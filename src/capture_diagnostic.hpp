#pragma once

// Private, one-run observation only; never a product or PR candidate.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <time.h>

namespace wls::capture_diagnostic {
enum class Function : std::size_t {
    ValidateEnvelope, CompiledContextFromJson, CanonicalJson,
    ParseJsonStrict, Sha256Hex, StatementStep
};
inline constexpr std::array<const char*, 6> names{
    "validate_envelope", "compiled_context_from_json", "canonical_json",
    "parse_json_strict", "sha256_hex", "Statement::step"};
struct Metric {
    std::uint64_t entries{}, exits{};
    std::int64_t inclusive_ns{}, self_ns{};
};
class Scope;
struct State {
    bool enabled{};
    Scope* top{};
    std::array<Metric, names.size()> metrics{};
    std::uint64_t snapshots{}, clock_errors{};
    std::int64_t wall_epoch_ns{}, cpu_epoch_ns{};
};
// External-linkage inline TLS: every participating translation unit uses
// the same per-thread state. Only the test's business main thread enables it.
inline thread_local State state{};

inline std::int64_t read_clock(clockid_t clock) noexcept {
    timespec value{};
    if (::clock_gettime(clock, &value) != 0) {
        ++state.clock_errors;
        return 0;
    }
    return static_cast<std::int64_t>(value.tv_sec) * 1000000000 +
        static_cast<std::int64_t>(value.tv_nsec);
}

class Scope {
public:
    explicit Scope(Function function) noexcept : function_(function) {
        if (!state.enabled) return;
        active_ = &state;
        parent_ = active_->top;
        ++active_->metrics[static_cast<std::size_t>(function_)].entries;
        started_ns_ = read_clock(CLOCK_MONOTONIC);
        active_->top = this;
    }
    ~Scope() noexcept {
        if (!active_) return;
        const auto elapsed = read_clock(CLOCK_MONOTONIC) - started_ns_;
        auto& metric = active_->metrics[static_cast<std::size_t>(function_)];
        ++metric.exits;
        metric.inclusive_ns += elapsed;
        metric.self_ns += elapsed - children_ns_;
        active_->top = parent_;
        if (parent_) parent_->children_ns_ += elapsed;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Function function_;
    State* active_{};
    Scope* parent_{};
    std::int64_t started_ns_{}, children_ns_{};
};

inline void enable() noexcept {
    state = State{};
    state.wall_epoch_ns = read_clock(CLOCK_MONOTONIC);
    state.cpu_epoch_ns = read_clock(CLOCK_THREAD_CPUTIME_ID);
    state.enabled = true;
}
inline void disable() noexcept { state.enabled = false; }

inline void begin(bool fixed, const char* mode, int round, const char* stage) noexcept {
    const auto wall = read_clock(CLOCK_MONOTONIC) - state.wall_epoch_ns;
    const auto cpu = read_clock(CLOCK_THREAD_CPUTIME_ID) - state.cpu_epoch_ns;
    std::fprintf(stderr, "NEXT_BEGIN fixed=%d mode=%s round=%d stage=%s wall_ns=%lld main_cpu_ns=%lld\n",
        static_cast<int>(fixed), mode, round, stage,
        static_cast<long long>(wall), static_cast<long long>(cpu));
    std::fflush(stderr);
}

inline void snapshot(bool fixed, const char* mode, int round, const char* stage) noexcept {
    if (state.top) {
        std::fprintf(stderr, "SNAPSHOT_REJECTED fixed=%d mode=%s round=%d stage=%s stack_empty=0\n",
            static_cast<int>(fixed), mode, round, stage);
        std::fflush(stderr);
        return;
    }
    const auto wall = read_clock(CLOCK_MONOTONIC) - state.wall_epoch_ns;
    const auto cpu = read_clock(CLOCK_THREAD_CPUTIME_ID) - state.cpu_epoch_ns;
    const auto sequence = ++state.snapshots;
    std::fprintf(stderr, "SNAPSHOT_BEGIN seq=%llu fixed=%d mode=%s round=%d stage=%s wall_ns=%lld main_cpu_ns=%lld stack_empty=1 clock_errors=%llu\n",
        static_cast<unsigned long long>(sequence), static_cast<int>(fixed), mode, round, stage,
        static_cast<long long>(wall), static_cast<long long>(cpu),
        static_cast<unsigned long long>(state.clock_errors));
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& metric = state.metrics[index];
        std::fprintf(stderr, "SNAPSHOT_METRIC seq=%llu function=%s entries=%llu exits=%llu inclusive_ns=%lld self_ns=%lld\n",
            static_cast<unsigned long long>(sequence), names[index],
            static_cast<unsigned long long>(metric.entries), static_cast<unsigned long long>(metric.exits),
            static_cast<long long>(metric.inclusive_ns), static_cast<long long>(metric.self_ns));
    }
    std::fprintf(stderr, "SNAPSHOT_END seq=%llu\n", static_cast<unsigned long long>(sequence));
    std::fflush(stderr);
}
} // namespace wls::capture_diagnostic
