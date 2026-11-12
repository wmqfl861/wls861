#pragma once

#include "wls/provider_profile.hpp"

#include <string_view>
#include <utility>

namespace wls::s1::response_metadata {
inline const std::set<std::string> response_fields{"id", "object", "status", "model", "output", "error", "incomplete_details", "usage",
    "background", "created_at", "completed_at", "instructions", "max_output_tokens", "parallel_tool_calls",
    "previous_response_id", "reasoning", "store", "temperature", "text", "tool_choice", "tools", "top_p",
    "truncation", "user", "metadata", "service_tier", "max_tool_calls", "top_logprobs", "prompt_cache_key",
    "prompt_cache_options", "prompt_cache_retention", "safety_identifier", "prompt_cache_diagnostics",
    "access_programs", "input", "reasoning_effort"};

// Paths only: raw originals retain values under the existing input byte limits.
class Collector {
public:
    explicit Collector(const ProviderProfile& profile) : enabled(!profile.response_metadata_policy.empty()) {}
    const bool enabled;
    void record(std::string_view parent, std::string_view key) {
        if (!enabled) return;
        ++observed_;
        std::string path(parent); path += '/';
        for (const char byte : key) {
            if (path.size() > max_bytes) { truncated_ = true; return; }
            if (byte == '~') path += "~0";
            else if (byte == '/') path += "~1";
            else path += byte;
        }
        if (paths_.contains(path)) return;
        if (paths_.size() == 128 || path.size() > max_bytes - bytes_) { truncated_ = true; return; }
        bytes_ += path.size(); paths_.insert(std::move(path));
    }
    void inspect(const boost::json::object& value, const std::set<std::string>& known, std::string_view path) {
        if (enabled) for (const auto& entry : value)
            if (!known.contains(std::string(entry.key()))) record(path, entry.key());
    }
    void response(const Json& value, const std::string& path) {
        if (!enabled || !value.is_object()) return;
        inspect(value.as_object(), response_fields, path);
        const auto* usage = value.as_object().if_contains("usage");
        if (!usage || !usage->is_object()) return;
        inspect(usage->as_object(), {"input_tokens", "output_tokens", "total_tokens", "input_tokens_details", "output_tokens_details"}, path + "/usage");
        for (const auto* name : {"input_tokens_details", "output_tokens_details"}) {
            const auto* details = usage->as_object().if_contains(name);
            if (details && details->is_object()) inspect(details->as_object(), std::string_view(name) == "input_tokens_details" ?
                std::set<std::string>{"cached_tokens", "cache_write_tokens"} : std::set<std::string>{"reasoning_tokens"}, path + "/usage/" + name);
        }
    }
    void write(boost::json::object& evidence) const {
        if (!enabled) return;
        boost::json::array paths;
        for (const auto& path : paths_) paths.emplace_back(path);
        evidence["unknown_fields"] = std::move(paths);
        evidence["unknown_fields_truncated"] = truncated_;
        evidence["unknown_fields_observed"] = observed_;
    }
private:
    static constexpr std::size_t max_bytes = 16U * 1024U;
    std::set<std::string> paths_;
    std::size_t bytes_{}, observed_{};
    bool truncated_{};
};
} // namespace wls::s1::response_metadata
