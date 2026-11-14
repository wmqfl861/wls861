#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace wls::s1::retry_detail {
// A retry consumes the original batch deadline; waiting never starts a new batch.
inline void wait(std::stop_token stop, std::chrono::steady_clock::time_point deadline,
                 std::chrono::seconds delay) {
    std::mutex mutex; std::unique_lock lock(mutex); std::condition_variable_any interrupted;
    interrupted.wait_until(lock, stop, std::min(deadline, std::chrono::steady_clock::now() + delay),
        [] { return false; });
}
} // namespace wls::s1::retry_detail
