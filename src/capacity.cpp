#include "wls/capacity.hpp"
#include <array>
#include <string>
namespace wls::s1 {
namespace {
using Object = boost::json::object;
std::int64_t integer(const Json& v) {
    if (v.is_int64()) return v.as_int64();
    if (v.is_uint64() && v.as_uint64() <= 100000000U) return static_cast<std::int64_t>(v.as_uint64());
    throw Error(ErrorCode::InvalidInput, "capacity requires bounded positive integers");
}
void need(bool ok, const char* why) { if (!ok) throw Error(ErrorCode::InvalidInput, why); }
}
Json store_capacity_limits(const Json& system) {
    const auto& limits = system.at("limits").as_object();
    Object result{{"business_events", 4096}, {"control_events", 8192}, {"operations", 1024},
        {"questions", 128}, {"open_questions", 32}, {"submissions_per_task", 64}, {"submissions", 256},
        {"acceptances", 256}, {"adverse_acceptances", 512}, {"deliveries", 64}, {"requirement_revisions", 100}};
    const auto* policy = limits.if_contains("store_capacity");
    if (!policy) return result;
    need(policy->is_object() && policy->as_object().size() == result.size(), "store_capacity requires every documented field");
    for (auto& field : result) {
        const auto* value = policy->as_object().if_contains(field.key());
        need(value != nullptr, "unknown or missing store_capacity field");
        const auto n = integer(*value);
        need(n > 0 && n <= 10000000, "store_capacity field is outside 1..10000000");
        field.value() = n;
    }
    need(result.at("control_events").as_int64() >= 64 && result.at("business_events").as_int64() >= 64,
        "event windows require at least 64 slots");
    need(result.at("open_questions").as_int64() <= result.at("questions").as_int64(), "open_questions exceeds questions");
    need(result.at("submissions_per_task").as_int64() <= result.at("submissions").as_int64(), "task submissions exceed request submissions");
    need(result.at("adverse_acceptances").as_int64() >= result.at("acceptances").as_int64(), "negative verdict reserve is below acceptance capacity");
    return result;
}
void validate_store_capacity_admission(const Json& system) {
    const auto capacity = store_capacity_limits(system);
    const auto& limits = system.at("limits");
    const auto teams = integer(limits.at("max_teams_per_request"));
    const auto calls = integer(limits.at("worker_max_tool_calls_per_order"));
    const auto rework = integer(limits.at("team_internal_rework_max"));
    const auto manager = integer(limits.at("manager_max_tool_calls_per_wake"));
    need(teams > 0 && teams <= 10000 && calls > 0 && calls <= 10000 && rework >= 0 && rework <= 10000 && manager > 0 && manager <= 12,
        "invalid task limits for capacity admission");
    const auto operations = teams * calls * (rework + 1);
    const auto management = teams * (rework + 1) * manager + 32;
    // Three admission/dispatch plus three settlement/feedback events per tool,
    // and independent management/exception reserves in each pool.
    need(capacity.at("operations").as_int64() >= operations, "store_capacity.operations cannot cover frozen team/order/rework limits");
    need(capacity.at("business_events").as_int64() >= operations * 3 + management &&
         capacity.at("control_events").as_int64() >= operations * 3 + management,
         "store_capacity event pools cannot cover six-event tool lifecycle and management reserve");
    need(capacity.at("submissions_per_task").as_int64() >= rework + 1 &&
         capacity.at("submissions").as_int64() >= teams * (rework + 1) &&
         capacity.at("acceptances").as_int64() >= teams * (rework + 1),
         "store_capacity cannot cover frozen submission/acceptance rounds");
}
}
