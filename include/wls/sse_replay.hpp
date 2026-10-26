#pragma once

#include "wls/model.hpp"

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <vector>

namespace wls::s1 {

struct ProviderProfile;

// One already-recorded transport read. elapsed is its nondecreasing arrival
// offset from the attempt's start, not a requested sleep or network timeout.
struct SseReplayChunk {
    std::string bytes;
    std::chrono::milliseconds elapsed{0};
    // An explicit recorded cancellation before this read; permits deterministic
    // mid-stream cancellation fixtures without sleeping or starting threads.
    bool cancel_before{false};
};

struct SseReplayLimits {
    std::size_t max_total_bytes{4U * 1024U * 1024U};
    std::size_t max_line_bytes{256U * 1024U};
    std::size_t max_event_bytes{1024U * 1024U};
    std::size_t max_events{4096};
    std::size_t max_chunks{16384};
    std::size_t max_output_items{128};
    std::size_t max_tool_calls{32};
    std::size_t max_content_parts{32};
    std::size_t max_text_bytes{256U * 1024U};
    std::size_t max_tool_argument_bytes{256U * 1024U};
};

struct ResponsesSseReplay {
    // A Completed response is still subject to the host's current Store tuple,
    // tool-schema and authorization guards. No replay method dispatches tools.
    ModelResponse response;
    Json evidence = boost::json::object{};
    // Exact consumed input bytes, including CR/LF/BOM/comments. Never a claim
    // that a network request occurred; the caller supplied all replay bytes.
    std::string raw_stream;
    Json terminal_response = nullptr;
    // Complete local report consistency only. Not a signed provider receipt,
    // proof of network execution, or an authorization against its own host.
    std::string record_sha256;
};

// Offline, bounded OpenAI Responses event subset. No sockets, threads, sleeps,
// retries or credential resolution. Partial tools are never returned for use.
// Invalid caller profile/request/limits throw Error; malformed transport and
// protocol responses return a tuple-preserving Error/Interrupted ModelResponse.
ResponsesSseReplay replay_responses_sse(const ProviderProfile& profile,
                                      const ModelRequest& request,
                                      const std::vector<SseReplayChunk>& chunks,
                                      SseReplayLimits limits = {},
                                      std::stop_token stop = {});

// Includes lossless base64 input and a digest, not an executed wire receipt.
Json to_json(const ResponsesSseReplay& replay);

} // namespace wls::s1
