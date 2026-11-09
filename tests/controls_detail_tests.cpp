#include "../src/controls_detail.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace wls::controls::detail;

constexpr std::string_view kBootIdA = "123e4567-e89b-12d3-a456-426614174000";
constexpr std::string_view kBootIdB = "00000000-0000-0000-0000-000000000001";
constexpr std::string_view kBootIdUppercase = "123E4567-E89B-12D3-A456-426614174000";
constexpr std::string_view kBootIdTooShort = "123e4567-e89b-12d3-a456-42661417400";
constexpr std::string_view kBootIdNotHex = "123e4567-e89b-12d3-a456-42661417400g";
constexpr std::string_view kBootIdDashMoved = "123e4567e-89b-12d3-a456-426614174000";

void set_boot_id(char (&destination)[boot_id_length + 1], std::string_view source) {
    CHECK(source.size() == boot_id_length);
    for (std::size_t i = 0; i < boot_id_length; ++i) destination[i] = source[i];
    destination[boot_id_length] = '\0';
}

clock_time seconds_only(std::int64_t seconds) { return clock_time{seconds, 0}; }

clock_sample valid_seam_sample() {
    clock_sample sample{};
    set_boot_id(sample.boot_id_before, kBootIdA);
    set_boot_id(sample.boot_id_after, kBootIdA);
    sample.boot_before = clock_time{100, 0};
    sample.realtime = clock_time{1'700'000'000, 123'456'789};
    sample.boot_after = clock_time{100, 0};
    return sample;
}

bool crosses(std::int64_t estimated, std::int64_t budget, std::int64_t p, std::int64_t q) {
    const threshold_compare_result result =
        compare_threshold_crossing(estimated, budget, positive_fraction{p, q});
    CHECK(result.status == threshold_compare_status::ok);
    return result.crossed;
}

void expect_parse_ok(std::string_view spelling, std::int64_t p, std::int64_t q) {
    const threshold_parse_result parsed = parse_threshold_fraction(spelling);
    CHECK(parsed.status == threshold_status::ok);
    CHECK(parsed.value.numerator == p);
    CHECK(parsed.value.denominator == q);
}

void expect_parse_status(std::string_view spelling, threshold_status status) {
    const threshold_parse_result parsed = parse_threshold_fraction(spelling);
    CHECK(parsed.status == status);
}

void threshold_parse_equal_value_spellings() {
    expect_parse_ok("0.8", 4, 5);
    for (const std::string_view spelling :
         {"0.80", "8e-1", "8E-1", "80e-2", "0.8e0", "0.8e+0", "0.8000", "0.8000000000000000"}) {
        expect_parse_ok(spelling, 4, 5);
    }
    expect_parse_ok("0.1", 1, 10);
    for (const std::string_view spelling :
         {"0.10", "1e-1", "10e-2", "1E-1", "0.1e+0", "0.10000000000000000000"}) {
        expect_parse_ok(spelling, 1, 10);
    }
}

void threshold_parse_domain_edges() {
    expect_parse_ok("0.01", 1, 100);
    expect_parse_ok("0.010", 1, 100);
    expect_parse_ok("0.99", 99, 100);
    expect_parse_ok("0.990", 99, 100);
    expect_parse_ok("0.08", 2, 25);
    expect_parse_ok("0.12345678901234567", 12345678901234567, 100000000000000000);
    expect_parse_ok("0.099999999999999999", 99999999999999999, 1000000000000000000);
}

void threshold_parse_sixtyfour_byte_boundary() {
    std::string legal64 = "0.8";
    legal64.append(61, '0');
    CHECK(legal64.size() == 64);
    expect_parse_ok(legal64, 4, 5);
    std::string legal64b = "0.01";
    legal64b.append(60, '0');
    CHECK(legal64b.size() == 64);
    expect_parse_ok(legal64b, 1, 100);
    std::string over65 = "0.8";
    over65.append(62, '0');
    CHECK(over65.size() == 65);
    expect_parse_status(over65, threshold_status::invalid_spelling);
}

void threshold_parse_rejects_invalid_spellings() {
    for (const std::string_view spelling :
         {"", ".8", "0.", "0.8.", "0.8e", "0.8e+", "0.8e-", "e1", "E1", "01", "00.8",
          "+0.8", " 0.8", "0.8 ", "0x8", "0.8d", "abc", "-", "0..8", "0.8e1.5", "1e",
          "1e+", "1.2e3e4", "--1", "0.8e++1"}) {
        expect_parse_status(spelling, threshold_status::invalid_spelling);
    }
}

void threshold_parse_rejects_negative() {
    for (const std::string_view spelling : {"-0.8", "-0.01", "-8e-1", "-0.99", "-1"}) {
        expect_parse_status(spelling, threshold_status::invalid_domain);
    }
}

void threshold_parse_rejects_out_of_domain() {
    for (const std::string_view spelling :
         {"1", "1.0", "1.5", "0", "0.0", "0.00", "0e-3", "0.001", "0.0099", "0.991",
          "0.9901", "0.8e1", "10e-1", "1e0"}) {
        expect_parse_status(spelling, threshold_status::invalid_domain);
    }
}

void threshold_parse_rejects_extreme_exponents() {
    for (const std::string_view spelling :
         {"1e999999", "1e-999999", "0.8e+999999", "0.8e-2147483648",
          "1e99999999999999999999", "0.8e-99999999999999999999"}) {
        expect_parse_status(spelling, threshold_status::invalid_exponent);
    }
}

void threshold_parse_checked_accumulation_boundary() {
    // 21 significant decimals: the exact reduced value would fit the p/q
    // bounds, but the raw significand exceeds the checked accumulator, so the
    // spelling is a typed invalid result by contract.
    expect_parse_status("0.102399999999999999896", threshold_status::invalid_domain);
    // 19 significant decimals fit the accumulator but exceed the p bound.
    expect_parse_status("0.1234567890123456789", threshold_status::invalid_domain);
}

void threshold_crossing_b1000_e799_800_801() {
    CHECK(!crosses(799, 1000, 4, 5));
    CHECK(crosses(800, 1000, 4, 5));
    CHECK(crosses(801, 1000, 4, 5));
    CHECK(!crosses(0, 1000, 4, 5));
    CHECK(crosses(1000, 1000, 4, 5));
}

void threshold_crossing_b_not_multiple_of_five() {
    // Independent small-domain oracle: 5*E >= 4*B cannot overflow here.
    CHECK(5 * 802 < 4 * 1003);
    CHECK(5 * 803 >= 4 * 1003);
    CHECK(!crosses(802, 1003, 4, 5));
    CHECK(crosses(803, 1003, 4, 5));
    CHECK(5 * 5 < 4 * 7);
    CHECK(5 * 6 >= 4 * 7);
    CHECK(!crosses(5, 7, 4, 5));
    CHECK(crosses(6, 7, 4, 5));
}

void threshold_crossing_small_domain_cross_oracle() {
    const positive_fraction fractions[] = {
        positive_fraction{4, 5},
        positive_fraction{1, 100},
        positive_fraction{99, 100},
        positive_fraction{12345678901234567, 100000000000000000},
    };
    for (const positive_fraction fraction : fractions) {
        for (std::int64_t budget = 1; budget <= 40; ++budget) {
            for (std::int64_t estimated = 0; estimated <= budget; ++estimated) {
                // Independent oracle: direct products in int64; all factors
                // stay small enough that no product can overflow.
                const bool expected =
                    estimated * fraction.denominator >= budget * fraction.numerator;
                CHECK(crosses(estimated, budget, fraction.numerator, fraction.denominator) ==
                      expected);
            }
        }
    }
}

void threshold_crossing_int64_neighbourhood() {
    const std::int64_t budget = std::numeric_limits<std::int64_t>::max();
    // Division-only oracles for the smallest crossing E:
    // p/q = 4/5   -> E = B - floor(B/5)
    // p/q = 99/100-> E = B - floor(B/100)
    // p/q = 1/100 -> E = ceil(B/100)
    const std::int64_t boundary_four_fifths = budget - budget / 5;
    CHECK(crosses(boundary_four_fifths, budget, 4, 5));
    CHECK(!crosses(boundary_four_fifths - 1, budget, 4, 5));
    const std::int64_t boundary_ninety_nine = budget - budget / 100;
    CHECK(crosses(boundary_ninety_nine, budget, 99, 100));
    CHECK(!crosses(boundary_ninety_nine - 1, budget, 99, 100));
    const std::int64_t boundary_hundredth = budget / 100 + (budget % 100 != 0 ? 1 : 0);
    CHECK(crosses(boundary_hundredth, budget, 1, 100));
    CHECK(!crosses(boundary_hundredth - 1, budget, 1, 100));
    CHECK(crosses(std::numeric_limits<std::int64_t>::max(), budget, 4, 5));
    CHECK(!crosses(0, budget, 99, 100));
    // Same boundaries on other large budgets (the alternating comparison
    // never forms p*B or q*E).
    const std::int64_t large_budgets[] = {1'000'000'000'000'000'000,
                                          999'999'999'999'999'999,
                                          123'456'789'123'456'789};
    for (const std::int64_t large : large_budgets) {
        const std::int64_t b45 = large - large / 5;
        CHECK(crosses(b45, large, 4, 5));
        CHECK(!crosses(b45 - 1, large, 4, 5));
    }
}

void threshold_zero_budget_and_invalid_inputs() {
    budget_threshold_result result =
        evaluate_budget_threshold(0, 0, positive_fraction{4, 5});
    CHECK(result.status == budget_threshold_status::zero_budget_no_threshold);
    result = evaluate_budget_threshold(5, 0, positive_fraction{4, 5});
    CHECK(result.status == budget_threshold_status::accounting_invariant);
    result = evaluate_budget_threshold(-1, 10, positive_fraction{4, 5});
    CHECK(result.status == budget_threshold_status::invalid_input);
    result = evaluate_budget_threshold(10, -1, positive_fraction{4, 5});
    CHECK(result.status == budget_threshold_status::invalid_input);
    result = evaluate_budget_threshold(10, 10, positive_fraction{4, 0});
    CHECK(result.status == budget_threshold_status::invalid_input);
    result = evaluate_budget_threshold(10, 10, positive_fraction{0, 10});
    CHECK(result.status == budget_threshold_status::invalid_input);
    result = evaluate_budget_threshold(10, 10, positive_fraction{-4, 5});
    CHECK(result.status == budget_threshold_status::invalid_input);
    result = evaluate_budget_threshold(799, 1000, positive_fraction{4, 5});
    CHECK(result.status == budget_threshold_status::ok && !result.crossed);
    result = evaluate_budget_threshold(800, 1000, positive_fraction{4, 5});
    CHECK(result.status == budget_threshold_status::ok && result.crossed);
}

void time_normalization_carry_and_borrow() {
    time_result normalized = normalize_time(1, 1'000'000'000);
    CHECK(normalized.status == time_status::ok);
    CHECK(normalized.value.seconds == 2 && normalized.value.nanoseconds == 0);
    normalized = normalize_time(1, -1);
    CHECK(normalized.status == time_status::ok);
    CHECK(normalized.value.seconds == 0 && normalized.value.nanoseconds == 999'999'999);
    normalized = normalize_time(0, 2'000'000'001);
    CHECK(normalized.status == time_status::ok);
    CHECK(normalized.value.seconds == 2 && normalized.value.nanoseconds == 1);
    normalized = normalize_time(-1, -999'999'999);
    CHECK(normalized.status == time_status::ok);
    CHECK(normalized.value.seconds == -2 && normalized.value.nanoseconds == 1);
    normalized = normalize_time(std::numeric_limits<std::int64_t>::max(), 1'000'000'000);
    CHECK(normalized.status == time_status::overflow);
}

void time_add_subtract_edges() {
    time_result added = add_time(clock_time{0, 999'999'999}, clock_time{0, 1});
    CHECK(added.status == time_status::ok);
    CHECK(added.value.seconds == 1 && added.value.nanoseconds == 0);
    added = add_time(seconds_only(std::numeric_limits<std::int64_t>::max()),
                     clock_time{0, 500'000'000});
    CHECK(added.status == time_status::ok && added.value.nanoseconds == 500'000'000);
    added = add_time(clock_time{std::numeric_limits<std::int64_t>::max(), 500'000'000},
                     clock_time{0, 500'000'000});
    CHECK(added.status == time_status::overflow);
    added = add_time(clock_time{0, 1'000'000'000}, seconds_only(1));
    CHECK(added.status == time_status::invalid_component);
    time_result subtracted = subtract_time(clock_time{2, 0}, clock_time{0, 1});
    CHECK(subtracted.status == time_status::ok);
    CHECK(subtracted.value.seconds == 1 && subtracted.value.nanoseconds == 999'999'999);
    subtracted = subtract_time(clock_time{0, 0}, clock_time{0, 1});
    CHECK(subtracted.status == time_status::ok);
    CHECK(subtracted.value.seconds == -1 && subtracted.value.nanoseconds == 999'999'999);
    subtracted = subtract_time(clock_time{0, 1}, clock_time{0, 1});
    CHECK(subtracted.status == time_status::ok);
    CHECK(subtracted.value.seconds == 0 && subtracted.value.nanoseconds == 0);
}

void time_compare_before_at_after() {
    const clock_time early = clock_time{10, 0};
    const clock_time at = clock_time{10, 0};
    const clock_time late = clock_time{10, 1};
    CHECK(compare_time(early, at) == 0);
    CHECK(compare_time(early, late) == -1);
    CHECK(compare_time(late, early) == 1);
    CHECK(compare_time(clock_time{9, 999'999'999}, clock_time{10, 0}) == -1);
}

void duration_minutes_bounds() {
    duration_result duration = wall_duration_from_minutes(1);
    CHECK(duration.status == duration_status::ok && duration.value.seconds == 60);
    duration = wall_duration_from_minutes(1'000'000'000'000);
    CHECK(duration.status == duration_status::ok);
    CHECK(duration.value.seconds == 60'000'000'000'000);  // 6*10^13 fits int64
    CHECK(duration.value.nanoseconds == 0);
    CHECK(wall_duration_from_minutes(0).status == duration_status::invalid_minutes);
    CHECK(wall_duration_from_minutes(-1).status == duration_status::invalid_minutes);
    CHECK(wall_duration_from_minutes(1'000'000'000'001).status ==
          duration_status::invalid_minutes);
}

void deadline_computation_and_repeatability() {
    const clock_sample sample = valid_seam_sample();
    admission_deadline_result admission = compute_admission_deadline(sample, 1);
    CHECK(admission.status == deadline_status::ok);
    CHECK(admission.value.boot_limit.seconds == 160);
    CHECK(admission.value.boot_limit.nanoseconds == 0);
    CHECK(admission.value.utc_limit.seconds == 1'700'000'060);
    CHECK(admission.value.utc_limit.nanoseconds == 123'456'789);
    CHECK(compare_time(admission.value.utc_created_at, sample.realtime) == 0);
    // The same stored anchor reproduces the same L: repeats, reopens, and
    // later batches in the same boot cannot refresh it.
    admission_deadline_result again = compute_admission_deadline(sample, 1);
    CHECK(again.status == deadline_status::ok);
    CHECK(compare_time(again.value.boot_limit, admission.value.boot_limit) == 0);
    // Maximum legal duration.
    admission = compute_admission_deadline(sample, 1'000'000'000'000);
    CHECK(admission.status == deadline_status::ok);
    CHECK(admission.value.boot_limit.seconds == 100 + 60'000'000'000'000);
    CHECK(admission.value.utc_limit.seconds == 1'700'000'000 + 60'000'000'000'000);
}

void deadline_utc_jump_keeps_boot_limit() {
    const clock_sample original = valid_seam_sample();
    clock_sample jumped = original;
    jumped.realtime = clock_time{2'000'000'000, 500'000'000};  // UTC moved; boot did not
    const admission_deadline_result first = compute_admission_deadline(original, 2);
    const admission_deadline_result second = compute_admission_deadline(jumped, 2);
    CHECK(first.status == deadline_status::ok && second.status == deadline_status::ok);
    CHECK(compare_time(first.value.boot_limit, second.value.boot_limit) == 0);
    CHECK(compare_time(first.value.utc_limit, second.value.utc_limit) != 0);
}

void deadline_gate_before_at_after() {
    const admission_deadline_result admission = compute_admission_deadline(valid_seam_sample(), 1);
    CHECK(admission.status == deadline_status::ok);
    const clock_time limit = admission.value.boot_limit;  // (160, 0)
    CHECK(evaluate_deadline(kBootIdA, limit, kBootIdA, clock_time{159, 999'999'999}) ==
          deadline_gate::within_limit);
    CHECK(evaluate_deadline(kBootIdA, limit, kBootIdA, clock_time{160, 0}) ==
          deadline_gate::expired);
    CHECK(evaluate_deadline(kBootIdA, limit, kBootIdA, clock_time{160, 1}) ==
          deadline_gate::expired);
    CHECK(evaluate_deadline(kBootIdA, limit, kBootIdB, clock_time{100, 0}) ==
          deadline_gate::boot_changed);
    CHECK(evaluate_deadline(kBootIdA, limit, kBootIdUppercase, clock_time{100, 0}) ==
          deadline_gate::invalid_input);
    CHECK(evaluate_deadline(kBootIdUppercase, limit, kBootIdA, clock_time{100, 0}) ==
          deadline_gate::invalid_input);
    CHECK(evaluate_deadline(kBootIdA, clock_time{160, 1'000'000'000}, kBootIdA,
                            clock_time{100, 0}) == deadline_gate::invalid_input);
    CHECK(evaluate_deadline(kBootIdA, limit, kBootIdA, clock_time{-1, 0}) ==
          deadline_gate::invalid_input);
}

void deadline_invalid_samples_and_minutes() {
    clock_sample reversed = valid_seam_sample();
    reversed.boot_after = clock_time{99, 999'999'999};
    CHECK(validate_clock_sample(reversed) == sample_status::boot_time_reversed);
    CHECK(compute_admission_deadline(reversed, 1).status == deadline_status::invalid_sample);

    clock_sample mismatched = valid_seam_sample();
    set_boot_id(mismatched.boot_id_after, kBootIdB);
    CHECK(validate_clock_sample(mismatched) == sample_status::boot_identity_mismatch);
    CHECK(compute_admission_deadline(mismatched, 1).status == deadline_status::invalid_sample);

    for (const std::string_view bad : {kBootIdUppercase, kBootIdNotHex, kBootIdDashMoved}) {
        clock_sample malformed = valid_seam_sample();
        set_boot_id(malformed.boot_id_before, bad);
        CHECK(validate_clock_sample(malformed) == sample_status::bad_identity_format);
        CHECK(compute_admission_deadline(malformed, 1).status == deadline_status::invalid_sample);
    }

    clock_sample negative = valid_seam_sample();
    negative.realtime = clock_time{-1, 0};
    CHECK(validate_clock_sample(negative) == sample_status::invalid_clock_component);

    clock_sample denormalized = valid_seam_sample();
    denormalized.boot_after = clock_time{100, 1'000'000'000};
    CHECK(validate_clock_sample(denormalized) == sample_status::invalid_clock_component);

    const clock_sample good = valid_seam_sample();
    CHECK(validate_clock_sample(good) == sample_status::ok);
    CHECK(compute_admission_deadline(good, 0).status == deadline_status::invalid_minutes);
    CHECK(compute_admission_deadline(good, -5).status == deadline_status::invalid_minutes);
    CHECK(compute_admission_deadline(good, 1'000'000'000'001).status ==
          deadline_status::invalid_minutes);

    clock_sample at_limit = valid_seam_sample();
    at_limit.boot_before = seconds_only(std::numeric_limits<std::int64_t>::max());
    at_limit.boot_after = seconds_only(std::numeric_limits<std::int64_t>::max());
    CHECK(compute_admission_deadline(at_limit, 1).status == deadline_status::overflow);
}

void boot_identity_fixed_format() {
    CHECK(boot_id_fixed_format_valid(kBootIdA));
    CHECK(boot_id_fixed_format_valid(kBootIdB));
    CHECK(!boot_id_fixed_format_valid(kBootIdUppercase));
    CHECK(!boot_id_fixed_format_valid(kBootIdTooShort));
    CHECK(!boot_id_fixed_format_valid(kBootIdNotHex));
    CHECK(!boot_id_fixed_format_valid(kBootIdDashMoved));
    CHECK(!boot_id_fixed_format_valid(""));
    std::string too_long(kBootIdA);
    too_long.push_back('0');
    CHECK(!boot_id_fixed_format_valid(too_long));
}

void remaining_milliseconds_floor_semantics() {
    milliseconds_result remaining =
        remaining_milliseconds_floor(clock_time{10, 0}, clock_time{10, 500'000});
    CHECK(remaining.status == time_status::ok && remaining.milliseconds == 0);
    remaining = remaining_milliseconds_floor(clock_time{10, 0}, clock_time{10, 1'000'000});
    CHECK(remaining.status == time_status::ok && remaining.milliseconds == 1);
    remaining = remaining_milliseconds_floor(clock_time{10, 999'999'999}, clock_time{11, 0});
    CHECK(remaining.status == time_status::ok && remaining.milliseconds == 0);
    remaining = remaining_milliseconds_floor(clock_time{11, 0}, clock_time{11, 0});
    CHECK(remaining.status == time_status::ok && remaining.milliseconds == 0);
    remaining = remaining_milliseconds_floor(clock_time{12, 0}, clock_time{11, 0});
    CHECK(remaining.status == time_status::ok && remaining.milliseconds == 0);
    remaining = remaining_milliseconds_floor(clock_time{100, 0},
                                             clock_time{60'000'000'000'100, 0});
    CHECK(remaining.status == time_status::ok &&
          remaining.milliseconds == 60'000'000'000'000'000LL);
    remaining = remaining_milliseconds_floor(clock_time{10, 1'000'000'000},
                                             clock_time{11, 0});
    CHECK(remaining.status == time_status::invalid_component);
}

// S0 boundary RED inputs prepared by the v16 test-only source stage (prepared
// here, not executed in that stage): now = {0,0} against bound seconds
// 9223372036854775 keeps the checked seconds*1000 product at
// 9223372036854775000, so the millisecond tail alone decides the last legal
// value. Each case is registered separately so one failure cannot mask the
// next; the legal minutes domain and every existing assertion stay unchanged.
void remaining_milliseconds_floor_int64_max_tail_807() {
    milliseconds_result remaining =
        remaining_milliseconds_floor(clock_time{0, 0}, clock_time{9223372036854775, 807'000'000});
    CHECK(remaining.status == time_status::ok);
    CHECK(remaining.milliseconds == std::numeric_limits<std::int64_t>::max());
}

void remaining_milliseconds_floor_tail_808_overflows() {
    milliseconds_result remaining =
        remaining_milliseconds_floor(clock_time{0, 0}, clock_time{9223372036854775, 808'000'000});
    CHECK(remaining.status == time_status::overflow);
}

void remaining_milliseconds_floor_tail_999_overflows() {
    milliseconds_result remaining =
        remaining_milliseconds_floor(clock_time{0, 0}, clock_time{9223372036854775, 999'000'000});
    CHECK(remaining.status == time_status::overflow);
}

void seam_sample_to_gate_integration() {
    // Trusted-seam contract: a well-formed injected sample validates, anchors
    // a deadline, and gates a now_boot inside the same boot identity. The
    // capture order itself (boot_id_before -> boot_before -> realtime ->
    // boot_after -> boot_id_after) is enforced by sample_host_clock in
    // controls_clock_posix.cpp; this seam test covers the validated contract.
    const clock_sample sample = valid_seam_sample();
    const admission_deadline_result admission = compute_admission_deadline(sample, 1);
    CHECK(admission.status == deadline_status::ok);
    const std::string_view admission_id(sample.boot_id_before, boot_id_length);
    CHECK(evaluate_deadline(admission_id, admission.value.boot_limit, admission_id,
                            sample.boot_after) == deadline_gate::within_limit);
    clock_sample later = sample;
    later.boot_after = clock_time{200, 0};
    CHECK(evaluate_deadline(admission_id, admission.value.boot_limit, admission_id,
                            later.boot_after) == deadline_gate::expired);
}

}  // namespace

int main() {
    const std::vector<::test::Case> cases = {
        {"threshold_parse_equal_value_spellings", threshold_parse_equal_value_spellings},
        {"threshold_parse_domain_edges", threshold_parse_domain_edges},
        {"threshold_parse_sixtyfour_byte_boundary", threshold_parse_sixtyfour_byte_boundary},
        {"threshold_parse_rejects_invalid_spellings", threshold_parse_rejects_invalid_spellings},
        {"threshold_parse_rejects_negative", threshold_parse_rejects_negative},
        {"threshold_parse_rejects_out_of_domain", threshold_parse_rejects_out_of_domain},
        {"threshold_parse_rejects_extreme_exponents", threshold_parse_rejects_extreme_exponents},
        {"threshold_parse_checked_accumulation_boundary",
         threshold_parse_checked_accumulation_boundary},
        {"threshold_crossing_b1000_e799_800_801", threshold_crossing_b1000_e799_800_801},
        {"threshold_crossing_b_not_multiple_of_five",
         threshold_crossing_b_not_multiple_of_five},
        {"threshold_crossing_small_domain_cross_oracle",
         threshold_crossing_small_domain_cross_oracle},
        {"threshold_crossing_int64_neighbourhood", threshold_crossing_int64_neighbourhood},
        {"threshold_zero_budget_and_invalid_inputs", threshold_zero_budget_and_invalid_inputs},
        {"time_normalization_carry_and_borrow", time_normalization_carry_and_borrow},
        {"time_add_subtract_edges", time_add_subtract_edges},
        {"time_compare_before_at_after", time_compare_before_at_after},
        {"duration_minutes_bounds", duration_minutes_bounds},
        {"deadline_computation_and_repeatability", deadline_computation_and_repeatability},
        {"deadline_utc_jump_keeps_boot_limit", deadline_utc_jump_keeps_boot_limit},
        {"deadline_gate_before_at_after", deadline_gate_before_at_after},
        {"deadline_invalid_samples_and_minutes", deadline_invalid_samples_and_minutes},
        {"boot_identity_fixed_format", boot_identity_fixed_format},
        {"remaining_milliseconds_floor_semantics", remaining_milliseconds_floor_semantics},
        {"remaining_milliseconds_floor_int64_max_tail_807", remaining_milliseconds_floor_int64_max_tail_807},
        {"remaining_milliseconds_floor_tail_808_overflows", remaining_milliseconds_floor_tail_808_overflows},
        {"remaining_milliseconds_floor_tail_999_overflows", remaining_milliseconds_floor_tail_999_overflows},
        {"seam_sample_to_gate_integration", seam_sample_to_gate_integration},
    };
    return ::test::run(cases);
}
