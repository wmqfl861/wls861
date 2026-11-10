#pragma once

// Private Controls W/D foundation helpers: exact rational budget thresholds
// and normalized clock-pair arithmetic with typed outcomes. This header is an
// internal source detail under src/, not a public API; tests include it via
// "../src/controls_detail.hpp". Everything here is pure: the only platform
// I/O lives in controls_clock_posix.cpp. Nothing in this header reads the
// ledger, grants reservations, or authorizes sends. Callers pass already
// captured canonical values: the estimated total E is spent+reserved from the
// real ledger (unknown cost is a subset of reserved, never added twice), and
// proving ledger consistency stays with the later integration stages.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

// The bounded streaming canonical digest below reuses the project JSON value
// and the process OpenSSL dependency; it stays pure with respect to business
// state (no ledger, reservation or authorization surface is touched).

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>

#include "wls/runtime_types.hpp"

namespace wls::controls::detail {

inline constexpr std::size_t max_threshold_spelling_bytes = 64;
inline constexpr std::uint64_t threshold_numerator_max = 99'999'999'999'999'999;
inline constexpr std::uint64_t threshold_denominator_max = 1'000'000'000'000'000'000;
// Exponent magnitudes beyond this bound cannot influence the accepted domain;
// they fail fast as a typed invalid result instead of saturating.
inline constexpr std::uint64_t threshold_exponent_magnitude_max = 4096;

inline constexpr std::int32_t nanoseconds_per_second = 1'000'000'000;
inline constexpr std::int64_t max_wall_minutes_min = 1;
inline constexpr std::int64_t max_wall_minutes_max = 1'000'000'000'000;
inline constexpr std::int64_t threshold_comparison_step_limit = 128;

inline constexpr std::size_t boot_id_length = 36;  // 32 hex digits + 4 dashes

// --- checked signed arithmetic, standard C++20 only ---

[[nodiscard]] constexpr bool add_i64_checked(std::int64_t a, std::int64_t b,
                                             std::int64_t& out) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) return false;
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) return false;
    out = a + b;
    return true;
}

[[nodiscard]] constexpr bool sub_i64_checked(std::int64_t a, std::int64_t b,
                                             std::int64_t& out) noexcept {
    if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) return false;
    if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) return false;
    out = a - b;
    return true;
}

[[nodiscard]] constexpr bool mul_i64_checked(std::int64_t a, std::int64_t b,
                                             std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    const std::uint64_t ma =
        a < 0 ? ~static_cast<std::uint64_t>(a) + 1 : static_cast<std::uint64_t>(a);
    const std::uint64_t mb =
        b < 0 ? ~static_cast<std::uint64_t>(b) + 1 : static_cast<std::uint64_t>(b);
    if ((a < 0) == (b < 0)) {
        if (ma > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / mb) {
            return false;
        }
    } else {
        constexpr std::uint64_t magnitude_of_int64_min = ~static_cast<std::uint64_t>(0) / 2 + 1;
        if (ma > magnitude_of_int64_min / mb) return false;
    }
    out = a * b;
    return true;
}

// --- normalized time pairs {seconds, nanoseconds} ---
// Durations stay in this split form; the largest legal wall duration
// (10^12 minutes = 6*10^13 seconds) is never folded into one int64 nanosecond
// count, which would overflow.

struct clock_time {
    std::int64_t seconds = 0;
    std::int32_t nanoseconds = 0;  // [0, nanoseconds_per_second) once normalized
};

enum class time_status : std::uint8_t {
    ok = 0,
    invalid_component,  // non-normalized input where normalization is required
    overflow,
};

struct time_result {
    time_status status = time_status::invalid_component;
    clock_time value = {};
};

[[nodiscard]] constexpr bool is_normalized_time(clock_time t) noexcept {
    return t.nanoseconds >= 0 && t.nanoseconds < nanoseconds_per_second;
}

[[nodiscard]] constexpr int compare_time(clock_time a, clock_time b) noexcept {
    // Both operands must be normalized.
    if (a.seconds != b.seconds) return a.seconds < b.seconds ? -1 : 1;
    if (a.nanoseconds != b.nanoseconds) return a.nanoseconds < b.nanoseconds ? -1 : 1;
    return 0;
}

inline time_result normalize_time(std::int64_t seconds, std::int64_t nanoseconds) {
    time_result result;
    std::int64_t quotient = nanoseconds / nanoseconds_per_second;
    std::int64_t remainder = nanoseconds % nanoseconds_per_second;
    if (remainder < 0) {
        remainder += nanoseconds_per_second;
        quotient -= 1;
    }
    std::int64_t total = 0;
    if (!add_i64_checked(seconds, quotient, total)) {
        result.status = time_status::overflow;
        return result;
    }
    result.status = time_status::ok;
    result.value.seconds = total;
    result.value.nanoseconds = static_cast<std::int32_t>(remainder);
    return result;
}

inline time_result add_time(clock_time a, clock_time b) {
    time_result result;
    if (!is_normalized_time(a) || !is_normalized_time(b)) return result;
    const std::int64_t combined =
        static_cast<std::int64_t>(a.nanoseconds) + static_cast<std::int64_t>(b.nanoseconds);
    const std::int64_t carry = combined / nanoseconds_per_second;
    const std::int64_t remainder = combined % nanoseconds_per_second;
    std::int64_t seconds = 0;
    if (!add_i64_checked(a.seconds, b.seconds, seconds) ||
        !add_i64_checked(seconds, carry, seconds)) {
        result.status = time_status::overflow;
        return result;
    }
    result.status = time_status::ok;
    result.value.seconds = seconds;
    result.value.nanoseconds = static_cast<std::int32_t>(remainder);
    return result;
}

inline time_result subtract_time(clock_time a, clock_time b) {
    time_result result;
    if (!is_normalized_time(a) || !is_normalized_time(b)) return result;
    std::int64_t nanoseconds =
        static_cast<std::int64_t>(a.nanoseconds) - static_cast<std::int64_t>(b.nanoseconds);
    std::int64_t seconds = 0;
    if (!sub_i64_checked(a.seconds, b.seconds, seconds)) {
        result.status = time_status::overflow;
        return result;
    }
    if (nanoseconds < 0) {
        nanoseconds += nanoseconds_per_second;
        if (!add_i64_checked(seconds, -1, seconds)) {
            result.status = time_status::overflow;
            return result;
        }
    }
    result.status = time_status::ok;
    result.value.seconds = seconds;
    result.value.nanoseconds = static_cast<std::int32_t>(nanoseconds);
    return result;
}

enum class duration_status : std::uint8_t {
    ok = 0,
    invalid_minutes,  // outside [1, 10^12]
    overflow,
};

struct duration_result {
    duration_status status = duration_status::invalid_minutes;
    clock_time value = {};
};

inline duration_result wall_duration_from_minutes(std::int64_t minutes) {
    duration_result result;
    if (minutes < max_wall_minutes_min || minutes > max_wall_minutes_max) return result;
    std::int64_t seconds = 0;
    if (!mul_i64_checked(minutes, 60, seconds)) {
        result.status = duration_status::overflow;
        return result;
    }
    result.status = duration_status::ok;
    result.value.seconds = seconds;
    return result;
}

// --- exact rational threshold ---

// Reduced positive fraction p/q produced only by parse_threshold_fraction.
struct positive_fraction {
    std::int64_t numerator = 0;    // p >= 1
    std::int64_t denominator = 0;  // q >= 1
};

enum class threshold_status : std::uint8_t {
    ok = 0,
    invalid_spelling,  // not a legal JSON decimal/exponent spelling, or > 64 bytes
    invalid_exponent,  // exponent magnitude outside the checked bound
    invalid_domain,    // value outside [0.01, 0.99], reduced p/q out of bounds,
                       // negative, or checked accumulation overflow
};

struct threshold_parse_result {
    threshold_status status = threshold_status::invalid_spelling;
    positive_fraction value = {};
};

// Parse an already captured canonical numeric spelling (at most 64 bytes,
// legal JSON decimal/exponent forms only) into the exact reduced p/q. The
// original spelling is never reconstructed through double or epsilon
// guessing. Leading redundant zeros are skipped and trailing zeros are
// stripped before checked accumulation, so equivalent spellings of the same
// value reduce identically; any accumulation or 10-power bound crossing is a
// typed invalid result, never a silent clamp.
inline threshold_parse_result parse_threshold_fraction(std::string_view spelling) {
    threshold_parse_result result;
    const std::string_view s = spelling;
    if (s.empty() || s.size() > max_threshold_spelling_bytes) return result;

    std::size_t i = 0;
    const bool negative = s[0] == '-';
    if (negative || s[0] == '+') {
        if (!negative) return result;  // '+' is not a legal JSON number prefix
        i = 1;
    }
    if (i >= s.size()) return result;

    // integer part: "0" | [1-9][0-9]*
    std::size_t int_digits = 0;
    if (s[i] == '0') {
        i += 1;
        int_digits = 1;
    } else if (s[i] >= '1' && s[i] <= '9') {
        const std::size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i += 1;
        int_digits = i - start;
    } else {
        return result;
    }

    // optional fraction part
    std::size_t frac_start = 0;
    std::size_t frac_digits = 0;
    if (i < s.size() && s[i] == '.') {
        i += 1;
        frac_start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i += 1;
        frac_digits = i - frac_start;
        if (frac_digits == 0) return result;
    }

    // optional exponent part
    std::int64_t exponent = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i += 1;
        bool exponent_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            exponent_negative = s[i] == '-';
            i += 1;
        }
        const std::size_t start = i;
        std::uint64_t magnitude = 0;
        bool magnitude_overflow = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            const std::uint64_t digit = static_cast<std::uint64_t>(s[i] - '0');
            if (!magnitude_overflow) {
                if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
                    magnitude_overflow = true;
                } else {
                    magnitude = magnitude * 10u + digit;
                }
            }
            i += 1;
        }
        if (i == start) return result;  // 'e' without digits
        if (magnitude_overflow || magnitude > threshold_exponent_magnitude_max) {
            result.status = threshold_status::invalid_exponent;
            return result;
        }
        exponent = static_cast<std::int64_t>(magnitude);
        if (exponent_negative) exponent = -exponent;
    }
    if (i != s.size()) return result;  // trailing content

    if (negative) {
        result.status = threshold_status::invalid_domain;
        return result;
    }

    // Exact value: digits / 10^scale with scale = frac_digits - exponent.
    const std::int64_t scale_raw = static_cast<std::int64_t>(frac_digits) - exponent;
    if (scale_raw <= 0) {
        result.status = threshold_status::invalid_domain;  // value >= 1 or zero
        return result;
    }

    char digits[max_threshold_spelling_bytes];
    std::size_t count = 0;
    const bool skip_leading_zero_int = int_digits == 1 && s[0] == '0';
    for (std::size_t k = skip_leading_zero_int ? 1 : 0; k < int_digits; ++k) {
        digits[count] = s[k];
        count += 1;
    }
    for (std::size_t k = 0; k < frac_digits; ++k) {
        digits[count] = s[frac_start + k];
        count += 1;
    }

    std::size_t begin = 0;
    while (begin < count && digits[begin] == '0') begin += 1;
    if (begin == count) {
        result.status = threshold_status::invalid_domain;  // value is zero
        return result;
    }
    std::size_t end = count;
    while (end > begin && digits[end - 1] == '0') end -= 1;
    const std::int64_t scale = scale_raw - static_cast<std::int64_t>(count - end);
    if (scale <= 0) {
        result.status = threshold_status::invalid_domain;  // value >= 1
        return result;
    }

    std::uint64_t significand = 0;
    for (std::size_t k = begin; k < end; ++k) {
        const std::uint64_t digit = static_cast<std::uint64_t>(digits[k] - '0');
        if (significand > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
            result.status = threshold_status::invalid_domain;  // checked accumulation bound
            return result;
        }
        significand = significand * 10u + digit;
    }

    // Reduce against 10^scale: the gcd of a decimal significand with 10^scale
    // only has factors 2 and 5.
    std::int64_t power2 = 0;
    std::int64_t power5 = 0;
    {
        std::uint64_t rest = significand;
        while ((rest & 1u) == 0) {
            rest >>= 1;
            power2 += 1;
        }
        while (rest % 5u == 0) {
            rest /= 5u;
            power5 += 1;
        }
    }
    const std::int64_t gcd2 = power2 < scale ? power2 : scale;
    const std::int64_t gcd5 = power5 < scale ? power5 : scale;
    std::uint64_t numerator = significand;
    for (std::int64_t k = 0; k < gcd2; ++k) numerator /= 2u;
    for (std::int64_t k = 0; k < gcd5; ++k) numerator /= 5u;
    if (numerator > threshold_numerator_max) {
        result.status = threshold_status::invalid_domain;
        return result;
    }

    // q = 10^scale / (2^gcd2 * 5^gcd5). Every remaining factor is >= 2, so the
    // product can only grow: once it passes 10^18 the reduced denominator is
    // out of bounds and the spelling is typed invalid.
    std::uint64_t denominator = 1;
    bool denominator_over = false;
    for (std::int64_t k = 0; k < scale - gcd2; ++k) {
        if (denominator > threshold_denominator_max / 2u) {
            denominator_over = true;
            break;
        }
        denominator *= 2u;
    }
    for (std::int64_t k = 0; k < scale - gcd5 && !denominator_over; ++k) {
        if (denominator > threshold_denominator_max / 5u) {
            denominator_over = true;
            break;
        }
        denominator *= 5u;
    }
    if (denominator_over) {
        result.status = threshold_status::invalid_domain;
        return result;
    }

    // Exact range check 0.01 <= p/q <= 0.99 using division only:
    // 100*p <= 99*q  <=>  floor(100p/q) < 99 || (floor == 99 && remainder == 0).
    const std::uint64_t hundred_times = numerator * 100u;
    if (hundred_times < denominator) {
        result.status = threshold_status::invalid_domain;  // value < 0.01
        return result;
    }
    const std::uint64_t hundred_quotient = hundred_times / denominator;
    const std::uint64_t hundred_remainder = hundred_times % denominator;
    if (hundred_quotient > 99u || (hundred_quotient == 99u && hundred_remainder != 0u)) {
        result.status = threshold_status::invalid_domain;  // value > 0.99
        return result;
    }

    result.status = threshold_status::ok;
    result.value.numerator = static_cast<std::int64_t>(numerator);
    result.value.denominator = static_cast<std::int64_t>(denominator);
    return result;
}

// --- threshold comparison and the zero-budget semantics ---

enum class threshold_compare_status : std::uint8_t {
    ok = 0,
    invalid_input,        // B <= 0, E < 0, or degenerate fraction (p < 1, q < 1)
    invariant_failure,    // alternating comparison exceeded the 128-step bound
};

struct threshold_compare_result {
    threshold_compare_status status = threshold_compare_status::invalid_input;
    bool crossed = false;  // meaningful only when status == ok
};

// Compare E/B >= p/q for already validated inputs using integer quotients and
// remainders only. When the integer quotients are equal the comparison
// continues on the reciprocals of the fractional remainders with the direction
// flipped; a zero remainder ends the comparison immediately. The products p*B
// and q*E are never formed, so E and B may use the full int64 range. The
// Euclid-style descent is bounded by threshold_comparison_step_limit; a
// descent past that bound is a typed invariant failure and is never reported
// as "not crossed".
inline threshold_compare_result compare_threshold_crossing(std::int64_t estimated_total_cost,
                                                            std::int64_t budget_total,
                                                            positive_fraction fraction) {
    threshold_compare_result result;
    if (budget_total <= 0 || estimated_total_cost < 0) return result;
    if (fraction.numerator < 1 || fraction.denominator < 1) return result;

    std::uint64_t a = static_cast<std::uint64_t>(estimated_total_cost);
    std::uint64_t b = static_cast<std::uint64_t>(budget_total);
    std::uint64_t c = static_cast<std::uint64_t>(fraction.numerator);
    std::uint64_t d = static_cast<std::uint64_t>(fraction.denominator);
    int direction = 1;  // sign of (a/b - c/d) at the current descent level
    for (std::int64_t step = 0; step < threshold_comparison_step_limit; ++step) {
        const std::uint64_t quotient_a = a / b;
        const std::uint64_t quotient_c = c / d;
        int comparison = 0;
        if (quotient_a != quotient_c) {
            comparison = quotient_a > quotient_c ? 1 : -1;
        } else {
            const std::uint64_t remainder_a = a % b;
            const std::uint64_t remainder_c = c % d;
            if (remainder_a == 0 && remainder_c == 0) {
                comparison = 0;
            } else if (remainder_a == 0) {
                comparison = -1;  // a/b is an integer, c/d is strictly larger
            } else if (remainder_c == 0) {
                comparison = 1;
            } else {
                // sign(ra/b - rc/d) = -sign(b/ra - d/rc): descend with flipped sign.
                a = b;
                b = remainder_a;
                c = d;
                d = remainder_c;
                direction = -direction;
                continue;
            }
        }
        result.status = threshold_compare_status::ok;
        result.crossed = direction * comparison >= 0;
        return result;
    }
    result.status = threshold_compare_status::invariant_failure;
    result.crossed = false;
    return result;
}

enum class budget_threshold_status : std::uint8_t {
    ok = 0,                    // B > 0 and E >= 0: crossed is meaningful
    zero_budget_no_threshold,  // B == 0 && E == 0: no percentage threshold exists
    accounting_invariant,      // B == 0 && E > 0: pre-existing over-budget conflict
    invalid_input,             // negative E/B or degenerate fraction
    invariant_failure,         // comparison exceeded the 128-step bound
};

struct budget_threshold_result {
    budget_threshold_status status = budget_threshold_status::invalid_input;
    bool crossed = false;  // meaningful only when status == ok
};

// Single entry point for the budget warning threshold. B == 0 keeps the
// original business behavior: E == 0 has no percentage threshold and no
// warning, while E > 0 is the pre-existing over-budget/accounting invariant
// conflict. Neither zero-budget case may be collapsed into crossed == false.
inline budget_threshold_result evaluate_budget_threshold(std::int64_t estimated_total_cost,
                                                         std::int64_t budget_total,
                                                         positive_fraction fraction) {
    budget_threshold_result result;
    if (estimated_total_cost < 0 || budget_total < 0) return result;
    if (budget_total == 0) {
        result.status = estimated_total_cost == 0
                            ? budget_threshold_status::zero_budget_no_threshold
                            : budget_threshold_status::accounting_invariant;
        return result;
    }
    const threshold_compare_result compared =
        compare_threshold_crossing(estimated_total_cost, budget_total, fraction);
    if (compared.status == threshold_compare_status::ok) {
        result.status = budget_threshold_status::ok;
        result.crossed = compared.crossed;
        return result;
    }
    if (compared.status == threshold_compare_status::invalid_input) return result;
    result.status = budget_threshold_status::invariant_failure;
    return result;
}

// --- boot identity and the trusted sampling seam ---

[[nodiscard]] inline bool boot_id_fixed_format_valid(std::string_view id) noexcept {
    if (id.size() != boot_id_length) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// Passive seam data captured strictly in this order by the host sampler:
// boot_id_before -> boot_before -> realtime -> boot_after -> boot_id_after.
// Tests inject these values explicitly; there is no environment, config, or
// model input that can redirect or retune a sampler.
struct clock_sample {
    char boot_id_before[boot_id_length + 1] = {};
    clock_time boot_before = {};
    clock_time realtime = {};
    clock_time boot_after = {};
    char boot_id_after[boot_id_length + 1] = {};
};

enum class sample_status : std::uint8_t {
    ok = 0,
    bad_identity_format,     // either id is not the fixed 36-char layout
    boot_identity_mismatch,  // boot_id_before != boot_id_after
    invalid_clock_component, // negative or non-normalized clock reading
    boot_time_reversed,      // boot_after < boot_before
};

[[nodiscard]] inline sample_status validate_clock_sample(const clock_sample& sample) noexcept {
    const std::string_view before(sample.boot_id_before, boot_id_length);
    const std::string_view after(sample.boot_id_after, boot_id_length);
    if (!boot_id_fixed_format_valid(before) || !boot_id_fixed_format_valid(after)) {
        return sample_status::bad_identity_format;
    }
    if (before != after) return sample_status::boot_identity_mismatch;
    if (sample.boot_before.seconds < 0 || sample.boot_after.seconds < 0 ||
        sample.realtime.seconds < 0) {
        return sample_status::invalid_clock_component;
    }
    if (!is_normalized_time(sample.boot_before) || !is_normalized_time(sample.boot_after) ||
        !is_normalized_time(sample.realtime)) {
        return sample_status::invalid_clock_component;
    }
    if (compare_time(sample.boot_after, sample.boot_before) < 0) {
        return sample_status::boot_time_reversed;
    }
    return sample_status::ok;
}

// POSIX-only host sampler; defined in controls_clock_posix.cpp. The fixed
// capture order is boot_id_before -> boot_before -> realtime -> boot_after ->
// boot_id_after, the boot identity path is a compile-time constant, and any
// failure is a typed unavailable result with no fallback clock.
enum class host_sample_status : std::uint8_t {
    ok = 0,
    unavailable,
};

struct host_sample_result {
    host_sample_status status = host_sample_status::unavailable;
    clock_sample sample = {};
};

host_sample_result sample_host_clock();

// --- admission deadline ---

struct boot_deadline {
    clock_time boot_before = {};     // admission anchor (early sample; no time gifted)
    clock_time boot_limit = {};      // L = boot_before + 60 * max_wall_minutes
    clock_time utc_created_at = {};  // W: the same realtime sample's UTC seconds
    clock_time utc_limit = {};       // D_utc = W + 60 * max_wall_minutes (calendar anchor)
};

enum class deadline_status : std::uint8_t {
    ok = 0,
    invalid_minutes,  // max_wall_minutes outside [1, 10^12]
    invalid_sample,   // the seam sample failed validation
    overflow,
};

struct admission_deadline_result {
    deadline_status status = deadline_status::invalid_sample;
    boot_deadline value = {};
};

// Derive the immutable deadline anchor from one validated sample. L is bound
// to the boot domain of the sampled boot identity: the same stored anchor
// always re-produces the same L, so a repeat, reopen, or later batch within
// the same boot can never refresh it. A different boot identity is rejected
// by evaluate_deadline instead of re-anchoring.
inline admission_deadline_result compute_admission_deadline(const clock_sample& sample,
                                                            std::int64_t max_wall_minutes) {
    admission_deadline_result result;
    if (validate_clock_sample(sample) != sample_status::ok) return result;
    const duration_result duration = wall_duration_from_minutes(max_wall_minutes);
    if (duration.status != duration_status::ok) {
        result.status = deadline_status::invalid_minutes;
        return result;
    }
    time_result limit = add_time(sample.boot_before, duration.value);
    if (limit.status != time_status::ok) {
        result.status = deadline_status::overflow;
        return result;
    }
    time_result utc_limit = add_time(sample.realtime, duration.value);
    if (utc_limit.status != time_status::ok) {
        result.status = deadline_status::overflow;
        return result;
    }
    result.status = deadline_status::ok;
    result.value.boot_before = sample.boot_before;
    result.value.boot_limit = limit.value;
    result.value.utc_created_at = sample.realtime;
    result.value.utc_limit = utc_limit.value;
    return result;
}

enum class deadline_gate : std::uint8_t {
    within_limit = 0,
    expired,        // same boot and now_boot >= L
    boot_changed,   // current boot identity differs from the admission anchor
    invalid_input,  // malformed identity or non-normalized/negative clock pair
};

// Expiry is only now_boot >= L on the admission boot. UTC jumps never move L;
// a different boot identity stops new business rather than re-anchoring.
inline deadline_gate evaluate_deadline(std::string_view admission_boot_id,
                                       clock_time boot_limit,
                                       std::string_view current_boot_id,
                                       clock_time now_boot) noexcept {
    if (!boot_id_fixed_format_valid(admission_boot_id) ||
        !boot_id_fixed_format_valid(current_boot_id)) {
        return deadline_gate::invalid_input;
    }
    if (admission_boot_id != current_boot_id) return deadline_gate::boot_changed;
    if (!is_normalized_time(boot_limit) || !is_normalized_time(now_boot) ||
        boot_limit.seconds < 0 || now_boot.seconds < 0) {
        return deadline_gate::invalid_input;
    }
    return compare_time(now_boot, boot_limit) >= 0 ? deadline_gate::expired
                                                   : deadline_gate::within_limit;
}

// Floor of the remaining margin in whole milliseconds. The caller must pass
// the minimum of all applicable absolute upper bounds. A margin below one
// millisecond yields zero so the caller stops; the value is never rounded up
// and never padded by an extra millisecond. The difference is computed from
// the split seconds/nanoseconds form, never as one int64 nanosecond count.
struct milliseconds_result {
    time_status status = time_status::invalid_component;
    std::int64_t milliseconds = 0;
};

inline milliseconds_result remaining_milliseconds_floor(clock_time now,
                                                        clock_time absolute_upper_bound) {
    milliseconds_result result;
    if (!is_normalized_time(now) || !is_normalized_time(absolute_upper_bound)) return result;
    if (now.seconds < 0 || absolute_upper_bound.seconds < 0) return result;
    if (compare_time(now, absolute_upper_bound) >= 0) {
        result.status = time_status::ok;  // no margin left: caller must stop
        return result;
    }
    std::int64_t seconds = 0;
    std::int64_t nanoseconds =
        static_cast<std::int64_t>(absolute_upper_bound.nanoseconds) -
        static_cast<std::int64_t>(now.nanoseconds);
    if (!sub_i64_checked(absolute_upper_bound.seconds, now.seconds, seconds)) {
        result.status = time_status::overflow;
        return result;
    }
    if (nanoseconds < 0) {
        nanoseconds += nanoseconds_per_second;
        if (!add_i64_checked(seconds, -1, seconds)) {
            result.status = time_status::overflow;
            return result;
        }
    }
    std::int64_t milliseconds = 0;
    if (!mul_i64_checked(seconds, 1000, milliseconds) ||
        !add_i64_checked(milliseconds, nanoseconds / 1'000'000, milliseconds)) {
        result.status = time_status::overflow;
        return result;
    }
    result.status = time_status::ok;
    result.milliseconds = milliseconds;
    return result;
}

// --- bounded streaming canonical digest (R03_REPORT) ---
//
// Digests the canonical serialization of a JSON value incrementally through a
// bounded chunk buffer without ever materializing the whole canonical string.
// The emitted byte stream matches the protected wls::s1 canonical reference
// exactly: object keys sorted, array order preserved, boost serializer
// key/scalar escaping, independently validated UTF-8, non-finite doubles
// rejected, and the same depth-128 typed limit. Sorting scratch is bounded by
// the strict-parser object-member cap and traversal recursion by the depth
// limit. All simultaneously retained key vectors share one scratch budget.

inline constexpr std::size_t canonical_stream_chunk_default = 64U * 1024U;
inline constexpr std::size_t canonical_stream_object_member_bound = 1048576U;
inline constexpr std::int64_t canonical_stream_unbounded = -1;

struct canonical_digest_result {
    bool ok = false;               // complete traversal with a verified digest
    bool oversize = false;         // stopped at max_bytes; exact total unknown
    bool overflow = false;         // the checked byte count itself overflowed
    std::int64_t bytes = 0;        // canonical bytes emitted (prefix when !ok)
    std::string sha256_hex_value;  // valid only when ok
};

namespace canonical_stream_internal {

struct sink {
    EVP_MD_CTX* context = nullptr;
    std::string buffer;
    std::size_t chunk_capacity = 0;
    std::int64_t emitted = 0;
    std::int64_t max_bytes = canonical_stream_unbounded;
    bool ok = true;
    bool oversize = false;
    bool overflow = false;
    std::size_t sorting_members = 0;

    void flush() {
        if (!ok || buffer.empty()) return;
        const auto chunk = static_cast<std::int64_t>(buffer.size());
        if (max_bytes >= 0 && emitted > max_bytes - chunk) {
            ok = false;
            oversize = true;
            buffer.clear();
            return;
        }
        if (EVP_DigestUpdate(context, buffer.data(), buffer.size()) != 1) {
            ok = false;
            buffer.clear();
            return;
        }
        std::int64_t next = 0;
        if (!add_i64_checked(emitted, chunk, next)) {
            ok = false;
            overflow = true;
            buffer.clear();
            return;
        }
        emitted = next;
        buffer.clear();
    }

    void emit(std::string_view bytes) {
        if (!ok) return;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto room = chunk_capacity - buffer.size();
            const auto take = std::min(room, bytes.size() - offset);
            buffer.append(bytes.substr(offset, take));
            offset += take;
            if (buffer.size() == chunk_capacity) flush();
            if (!ok) return;
        }
    }
};

inline void validate_utf8(boost::json::string_view text) {
    std::uint32_t code = 0, minimum = 0;
    unsigned remaining = 0;
    for (const unsigned char byte : text) {
        if (remaining) {
            if ((byte & 0xc0) != 0x80) throw std::invalid_argument("invalid UTF-8 continuation");
            code = (code << 6) | (byte & 0x3f);
            if (--remaining == 0 && (code < minimum || code > 0x10ffff ||
                                      (code >= 0xd800 && code <= 0xdfff)))
                throw std::invalid_argument("invalid UTF-8 scalar");
        } else if (byte >= 0x80) {
            if (byte >= 0xc2 && byte <= 0xdf) { remaining = 1; minimum = 0x80; code = byte & 0x1f; }
            else if (byte <= 0xef && byte >= 0xe0) { remaining = 2; minimum = 0x800; code = byte & 0x0f; }
            else if (byte <= 0xf4 && byte >= 0xf0) { remaining = 3; minimum = 0x10000; code = byte & 7; }
            else throw std::invalid_argument("invalid UTF-8 leading byte");
        }
    }
    if (remaining) throw std::invalid_argument("incomplete UTF-8 scalar");
}

// Resetting a serializer on one scalar/string retains no document-sized
// output. Partial string escaping uses only the fixed serializer/chunk state.
inline void scalar(const wls::s1::Json* value, boost::json::string_view key, sink& out) {
    if (!value || value->is_string()) validate_utf8(value ? boost::json::string_view(value->as_string()) : key);
    unsigned char scratch[512];
    boost::json::serializer serializer(boost::json::get_null_resource(), scratch, sizeof scratch);
    if (value) serializer.reset(value);
    else serializer.reset(key);
    char bytes[1024];
    while (out.ok && !serializer.done()) {
        const auto chunk = serializer.read(bytes);
        out.emit(std::string_view(chunk.data(), chunk.size()));
    }
}

inline void walk(const wls::s1::Json& value, sink& out, std::size_t depth) {
    if (!out.ok) return;
    if (depth > 128 || (depth == 128 && (value.is_array() || value.is_object())))
        throw wls::s1::Error(wls::s1::ErrorCode::InvalidInput,
                                          "JSON exceeds canonical depth limit");
    if (value.is_object()) {
        const auto count = value.as_object().size();
        if (count > canonical_stream_object_member_bound - out.sorting_members)
            throw wls::s1::Error(wls::s1::ErrorCode::InvalidInput,
                                 "streaming canonical object exceeds sorting bound");
        out.sorting_members += count;
        struct release { sink& out; std::size_t count; ~release() { out.sorting_members -= count; } } retained{out, count};
        std::vector<const boost::json::key_value_pair*> entries;
        entries.reserve(count);  // aggregate budget checked BEFORE allocation
        for (const auto& member : value.as_object()) entries.push_back(&member);
        std::sort(entries.begin(), entries.end(),
                  [](const auto* left, const auto* right) { return left->key() < right->key(); });
        out.emit("{");
        bool first = true;
        for (const auto* member : entries) {
            if (!first) out.emit(",");
            first = false;
            scalar(nullptr, member->key(), out);
            out.emit(":");
            walk(member->value(), out, depth + 1);
            if (!out.ok) return;
        }
        out.emit("}");
    } else if (value.is_array()) {
        out.emit("[");
        bool first = true;
        for (const auto& element : value.as_array()) {
            if (!first) out.emit(",");
            first = false;
            walk(element, out, depth + 1);
            if (!out.ok) return;
        }
        out.emit("]");
    } else {
        if (value.is_double() && !std::isfinite(value.as_double()))
            throw wls::s1::Error(wls::s1::ErrorCode::InvalidInput, "non-finite JSON number");
        scalar(&value, {}, out);
    }
}

}  // namespace canonical_stream_internal

// Stream the canonical serialization of `value` into SHA-256 through bounded
// chunks. With max_bytes >= 0 the traversal stops as soon as the canonical
// byte count would exceed it (oversize=true, exact total unknown); the caller
// reports the affected length/hash as null, never zero.
inline canonical_digest_result canonical_sha256_stream(const wls::s1::Json& value,
        std::size_t chunk_bytes = canonical_stream_chunk_default,
        std::int64_t max_bytes = canonical_stream_unbounded) {
    canonical_digest_result result;
    if (chunk_bytes == 0) chunk_bytes = canonical_stream_chunk_default;
    if (chunk_bytes > canonical_stream_chunk_default) return result;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) return result;
    canonical_stream_internal::sink state{context.get(), std::string{}, chunk_bytes, 0, max_bytes,
                                          true, false, false};
    state.buffer.reserve(chunk_bytes);
    try {
        canonical_stream_internal::walk(value, state, 0);
    } catch (const std::exception&) {
        state.ok = false;
    }
    if (state.ok) state.flush();
    else state.buffer.clear();
    result.bytes = state.emitted;
    if (!state.ok) {
        result.oversize = state.oversize;
        result.overflow = state.overflow;
        return result;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != 32) return result;
    constexpr char hex[] = "0123456789abcdef";
    result.sha256_hex_value.reserve(64);
    for (unsigned int i = 0; i < size; ++i) {
        result.sha256_hex_value.push_back(hex[digest[i] >> 4]);
        result.sha256_hex_value.push_back(hex[digest[i] & 15]);
    }
    result.ok = true;
    return result;
}

// --- checked run totals accumulator (R03_REPORT / F4_COUNTS) ---
//
// Validated nonnegative integer accumulation for the bounded report totals.
// An overflow, a negative/malformed observation or a missing completeness
// fact never mutates the published known prefix: the affected totals become
// unknown with completeness false, and the retained safe prefix stays
// available only as an explicitly labelled lower bound.

struct run_totals {
    std::int64_t attempts = 0;
    std::int64_t new_reservations = 0;
    std::int64_t network_started_calls = 0;
    bool counts_complete = true;
    bool network_count_complete = true;

    // Checked nonnegative delta of two durable observations. A negative or
    // overflowing difference marks the observation unknown instead of
    // producing a fabricated value.
    static std::int64_t checked_delta(std::int64_t after, std::int64_t before, bool& ok) {
        std::int64_t delta = 0;
        if (!ok || after < before || before < 0 || !sub_i64_checked(after, before, delta)) {
            ok = false;
            return 0;
        }
        return delta;
    }

    void add_attempts(std::int64_t delta) {
        std::int64_t next = 0;
        if (delta < 0 || !counts_complete || !add_i64_checked(attempts, delta, next)) {
            counts_complete = false;
            return;
        }
        attempts = next;
    }

    void add_reservation(std::int64_t delta) {
        std::int64_t next = 0;
        if (delta < 0 || !counts_complete || !add_i64_checked(new_reservations, delta, next)) {
            counts_complete = false;
            return;
        }
        new_reservations = next;
    }

    void observe_network(std::int64_t delta, bool observed_complete, bool live) {
        if (!live) return;  // Fake network zero is proven by the execution type
        if (delta < 0 || !observed_complete) {
            network_count_complete = false;
            return;
        }
        std::int64_t next = 0;
        if (!network_count_complete || !add_i64_checked(network_started_calls, delta, next)) {
            network_count_complete = false;
            return;
        }
        network_started_calls = next;
    }
};

}  // namespace wls::controls::detail
