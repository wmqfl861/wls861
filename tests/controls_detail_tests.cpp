#include "../src/controls_detail.hpp"
#include "test_support.hpp"
#include "live_test_support.hpp"

// Exercise the actual production driver/output, including its real main
// error boundary, in this test translation unit. No substitute implementation.
#define main controls_embedded_cli_main
#include "../src/cli.cpp"
#undef main
#include <sstream>

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

// R03_REPORT / F4_BOUNDS.1 permanent fixture (NOT_RUN in this source-only
// stage): the bounded streaming canonical digest equals the protected
// canonical reference byte-for-byte for escaped strings, UTF-8, nested
// differently inserted object keys, arrays and finite numeric spellings, and
// exercises chunk rollover through small chunk sizes. This is the same
// production helper the CLI driver uses for its raw pump-report digest.
void streaming_canonical_digest_matches_reference() {
    using wls::s1::canonical_json;
    using wls::s1::parse_json_strict;
    using wls::s1::sha256_hex;
    const char* documents[] = {
        "{\"b\":1,\"a\":[1,2,3],\"c\":\"plain\"}",
        "{\"esc\":\"quote\\\" backslash\\\\ slash\\/ \\u0001 control\"}",
        "{\"utf8\":\"\\u5c71\\u4e2d raw-\\u00e9 text\"}",
        "{\"nested\":{\"z\":1,\"a\":{\"m\":[true,false,null]}}}",
        "{\"numbers\":[0,-0.0,1.5,2e3,1.2345678901234568,1024,-1e-9]}",
        "{\"k\":\"\\u7eaj\\u7b14\\u5c5e\\u4e8e\\u5c71\\u4e2d\\uff0c\\u800c\\u4e0d\\u662f\\u5c71\"}",
    };
    for (const char* document : documents) {
        const auto value = parse_json_strict(document);
        const auto reference = canonical_json(value);
        const auto expected = sha256_hex(reference);
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{64},
                                        std::size_t{4096}, canonical_stream_chunk_default}) {
            const auto result = canonical_sha256_stream(value, chunk);
            CHECK(result.ok);
            CHECK(!result.oversize && !result.overflow);
            CHECK(result.sha256_hex_value == expected);
            CHECK(result.bytes == static_cast<std::int64_t>(reference.size()));
        }
    }
    // Differently inserted keys of equal objects canonicalize identically.
    const auto ordered = parse_json_strict("{\"x\":1,\"y\":{\"b\":2,\"a\":3}}");
    const auto rotated = parse_json_strict("{\"y\":{\"a\":3,\"b\":2},\"x\":1}");
    const auto left = canonical_sha256_stream(ordered);
    const auto right = canonical_sha256_stream(rotated);
    CHECK(left.ok && right.ok && left.sha256_hex_value == right.sha256_hex_value);
    // Canonical depth behavior: deep-but-legal folds fine and matches the
    // reference; one level beyond the limit fails typed in the protected
    // reference and the streaming walker alike (never a digest of something
    // else).
    std::string deep = "{\"v\":";
    for (int level = 0; level < 126; ++level) deep += "{\"v\":";
    deep += "1";
    for (int level = 0; level < 126; ++level) deep += "}";
    deep += "}";
    const auto deep_value = parse_json_strict(deep, 65536, 256);
    const auto deep_result = canonical_sha256_stream(deep_value);
    CHECK(deep_result.ok);
    CHECK(deep_result.sha256_hex_value == sha256_hex(canonical_json(deep_value)));
    std::string too_deep = "{\"v\":{\"v\":" + deep + "}}";  // 129 levels: past the limit
    bool reference_threw = false;
    try { (void)canonical_json(parse_json_strict(too_deep, 65536, 256)); }
    catch (const wls::s1::Error&) { reference_threw = true; }
    CHECK(reference_threw);
    const auto deep_failed = canonical_sha256_stream(parse_json_strict(too_deep, 65536, 256));
    CHECK(!deep_failed.ok && !deep_failed.oversize && !deep_failed.overflow);
    // max_bytes bound: an early stop reports oversize with only the flushed
    // prefix counted; the exact total stays unknown, never fabricated.
    std::string long_scalar(200, 'x');
    const auto bounded = parse_json_strict("{\"s\":\"" + long_scalar + "\"}");
    const auto stopped = canonical_sha256_stream(bounded, canonical_stream_chunk_default, 32);
    CHECK(!stopped.ok && stopped.oversize);
    CHECK(stopped.bytes <= 32);
    CHECK(stopped.sha256_hex_value.empty());
}

// R03_REPORT / F4_COUNTS.4 permanent fixture (NOT_RUN here): checked
// nonnegative integer accumulation for the report totals. Overflow never
// mutates the published known-prefix accumulator; negative or malformed
// deltas and missing live observations mark the affected totals unknown with
// completeness false while the retained prefix stays available.
void run_totals_checked_boundaries() {
    run_totals totals;
    totals.add_attempts(5);
    totals.add_reservation(5);
    CHECK(totals.attempts == 5 && totals.new_reservations == 5 && totals.counts_complete);
    totals.add_attempts(0);  // a zero delta is a legal observed batch
    CHECK(totals.counts_complete);
    bool ok = true;
    CHECK(run_totals::checked_delta(7, 5, ok) == 2 && ok);
    CHECK(run_totals::checked_delta(5, 7, ok) == 0 && !ok);        // reversed clock
    ok = true;
    CHECK(run_totals::checked_delta(-1, 0, ok) == 0 && !ok);       // negative observation
    CHECK(run_totals::checked_delta(std::numeric_limits<std::int64_t>::max(), 0, ok) == 0 && !ok);
    {   // int64 boundary addition overflows without corrupting the prefix
        run_totals edge;
        edge.add_attempts(std::numeric_limits<std::int64_t>::max() - 3);
        CHECK(edge.counts_complete);
        edge.add_attempts(10);
        CHECK(!edge.counts_complete);
        CHECK(edge.attempts == std::numeric_limits<std::int64_t>::max() - 3);  // prefix unmutated
        edge.add_attempts(1);  // once unknown, further adds never re-complete
        CHECK(!edge.counts_complete);
    }
    {   // network coverage: complete observations accumulate checked; a
        // missing/partial observation makes the total unknown forever; Fake
        // (non-live) execution contributes nothing and stays complete-zero.
        run_totals live;
        live.observe_network(31, true, true);
        wls::test::live::controls_regression_evidence("network-helper-known31", Object{
            {"network_started_calls", live.network_started_calls}, {"complete", live.network_count_complete}});
        CHECK(live.network_started_calls == 31 && live.network_count_complete);
        live.observe_network(0, true, true);
        CHECK(live.network_count_complete);
        live.observe_network(5, false, true);  // five known sends plus missing observations
        wls::test::live::controls_regression_evidence("network-helper-partial5", Object{
            {"network_started_calls", live.network_started_calls}, {"complete", live.network_count_complete}});
        CHECK(!live.network_count_complete && live.network_started_calls == 36);
        live.observe_network(7, true, true);   // a later good batch cannot heal
        wls::test::live::controls_regression_evidence("network-helper-later7", Object{
            {"network_started_calls", live.network_started_calls}, {"complete", live.network_count_complete}});
        CHECK(!live.network_count_complete && live.network_started_calls == 43);
        run_totals fake;
        fake.observe_network(0, false, false);
        wls::test::live::controls_regression_evidence("network-helper-fake", Object{
            {"network_started_calls", fake.network_started_calls}, {"complete", fake.network_count_complete}});
        CHECK(fake.network_started_calls == 0 && fake.network_count_complete);
        run_totals overflow;
        overflow.network_started_calls = std::numeric_limits<std::int64_t>::max() - 1;
        overflow.observe_network(5, true, true);
        wls::test::live::controls_regression_evidence("network-helper-overflow", Object{
            {"network_started_calls", overflow.network_started_calls}, {"complete", overflow.network_count_complete}});
        CHECK(!overflow.network_count_complete);
        CHECK(overflow.network_started_calls == std::numeric_limits<std::int64_t>::max() - 1);
    }
}

void streaming_program_created_utf8_and_depth_boundaries() {
    const auto agrees = [](const Json& value, bool expected) {
        bool accepted = true;
        std::string reference;
        try { reference = canonical_json(value); }
        catch (const Error&) { accepted = false; }
        CHECK(accepted == expected);
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{1024}, canonical_stream_chunk_default}) {
            const auto result = canonical_sha256_stream(value, chunk);
            CHECK(result.ok == expected);
            if (expected) {
                CHECK(result.bytes == static_cast<std::int64_t>(reference.size()));
                CHECK(result.sha256_hex_value == sha256_hex(reference));
            } else CHECK(result.sha256_hex_value.empty());
        }
    };
    for (const auto& invalid : {std::string("\xc0\xaf", 2), std::string("\xe2\x28\xa1", 3),
            std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)}) {
        agrees(Json(invalid), false);
        Object bad_key;
        bad_key.emplace(boost::json::string_view(invalid.data(), invalid.size()), 1);
        agrees(bad_key, false);
        agrees(Array{Object{{"value", invalid}}}, false);
    }
    std::string key(131072, '\\'), value(196608, '\t');
    key += "\xf0\x9f\x8c\xb1";
    value += "\xe5\xb1\xb1\"\\\n";
    Object long_strings;
    long_strings.emplace(boost::json::string_view(key.data(), key.size()), value);
    agrees(long_strings, true);
    CHECK(!canonical_sha256_stream(long_strings, canonical_stream_chunk_default + 1).ok);
    for (const int depth : {127, 128, 129}) {
        Json object_value = 1, array_value = 1;
        for (int i = 0; i < depth; ++i) {
            object_value = Object{{"value", std::move(object_value)}};
            array_value = Array{std::move(array_value)};
        }
        agrees(object_value, depth <= 128);
        agrees(array_value, depth <= 128);
    }
}

namespace live_fixture = wls::test::live;

struct DriverFixture {
    live_fixture::Assets assets{WLS_SOURCE_DIR};
    live_fixture::Fixture f;
    explicit DriverFixture(bool live = false)
        : f(assets, live_fixture::settings(), live, 410, !live) {}
};

void streaming_aggregate_sorting_scratch_boundary() {
    const auto half = canonical_stream_object_member_bound / 2;
    Object outer, inner;
    outer.reserve(half); inner.reserve(half + 1);
    for (std::size_t index = 0; index < half; ++index) {
        const auto key = "z" + std::to_string(index);
        inner.emplace(key, 0);
        if (index + 1 < half) outer.emplace(key, 0);
    }
    outer.emplace("!retained_nested_scratch", std::move(inner));
    CHECK(outer.size() == half);
    CHECK(outer.at("!retained_nested_scratch").as_object().size() == half);
    const auto exact = canonical_sha256_stream(outer);
    CHECK(exact.ok);
    const auto reference = canonical_json(outer);
    CHECK(exact.bytes == static_cast<std::int64_t>(reference.size()));
    CHECK(exact.sha256_hex_value == sha256_hex(reference));
    // Parent sorting entries remain live while the first sorted child is
    // traversed: one more child's member exceeds the aggregate by one.
    outer.at("!retained_nested_scratch").as_object().emplace("one-extra-member", 0);
    const auto exhausted = canonical_sha256_stream(outer);
    CHECK(!exhausted.ok && !exhausted.oversize && !exhausted.overflow);
    CHECK(exhausted.sha256_hex_value.empty());
}

Json driver_empty_report(std::string_view stop = "quiescent") {
    return Object{{"calls", Array{}}, {"blocked", Array{}}, {"manager_calls", 0},
        {"network_started_calls", 0}, {"network_count_complete", true}, {"stop_reason", stop}};
}
bool driver_contains(const Json& values, const char* expected) {
    for (const auto& value : values.as_array()) if (value == expected) return true;
    return false;
}

void driver_throw_observes_original_reservations() {
    for (const bool reserve_first : {false, true}) {
        DriverFixture d;
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
            [&](std::int64_t) -> Json {
                ++invocations;
                if (reserve_first) (void)d.f.reserve(d.f.request("throw-original"));
                throw std::runtime_error("real callback exception");
            });
        CHECK(invocations == 1);
        CHECK(report.at("batches_started") == 1 && report.at("batches_completed") == 0);
        CHECK(report.at("report_complete") == false);
        CHECK(report.at("counts_complete") == true);
        CHECK(report.at("attempts_total") == (reserve_first ? 1 : 0));
        CHECK(report.at("new_reservations_total") == (reserve_first ? 1 : 0));
        CHECK(report.at("network_started_calls") == 0);
        const auto& summary = report.at("batch_summaries").as_array().front();
        CHECK(summary.at("completed") == false);
        CHECK(summary.at("report_sha256").is_null());
        CHECK(summary.at("new_attempts") == (reserve_first ? 1 : 0));
        CHECK(summary.at("attempt_set_sha256").is_string());
        if (reserve_first) {
            CHECK(live_fixture::number(d.f.store->attempt("throw-original"), "state_version") == 1);
            CHECK(report.at("history_complete") == false);
        }
    }
}

void driver_throw_interval_excludes_prerun_and_foreign_events() {
    DriverFixture d;
    const auto prior_events = d.f.store->events(d.f.request_id, 0, 1000);
    CHECK(!prior_events.empty());
    const auto prior_highwater = live_fixture::number(prior_events.back(), "seq");
    std::int64_t invocations = 0;
    std::string sibling;
    const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
        [&](std::int64_t) -> Json {
            ++invocations;
            sibling = live_fixture::text(d.f.store->admit(
                live_fixture::admission(d.assets, d.f.live, "foreign-event-gap", false, true)), "request_id");
            (void)d.f.reserve(d.f.request("interval-current-original"));
            throw std::runtime_error("throw after original reservation and foreign-request gap");
        });
    CHECK(invocations == 1 && report.at("batches_started") == 1 && report.at("batches_completed") == 0);
    const auto actual = d.f.store->events(d.f.request_id, prior_highwater, 1000);
    CHECK(!actual.empty());
    const auto first = live_fixture::number(actual.front(), "seq");
    const auto last = live_fixture::number(actual.back(), "seq");
    CHECK(first > prior_highwater);
    CHECK(!d.f.store->events(sibling, prior_highwater, 1000).empty());
    const auto& summary = report.at("batch_summaries").as_array().front();
    CHECK(summary.at("event_seq_first") == first && summary.at("event_seq_last") == last);
    CHECK(summary.at("attempt_set_sha256") == sha256_hex(canonical_json(Array{"interval-current-original"})));
    CHECK(summary.at("new_attempts") == 1 && report.at("attempts_total") == 1);
    CHECK(summary.at("report_sha256").is_null() && summary.at("report_bytes").is_null());
}

void driver_live_oversize_retains_network_or_unknown() {
    for (const bool observed : {false, true}) {
        DriverFixture d(true);
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, false, Object{},
            [&](std::int64_t) {
                ++invocations;
                const auto request = d.f.request("oversize-live-original");
                (void)d.f.reserve(request);
                const auto claim = d.f.claim(request);
                if (observed) {
                    // Synthetic original transport fact in this disposable
                    // Store only; no TCP/provider operation or permission bypass.
                    live_fixture::sql(d.f.path, "INSERT INTO live_transport_observations VALUES ('" + request.attempt_id +
                        "','" + live_fixture::text(claim, "claim_id") + "','" + sha256_hex("synthetic observed transport") +
                        "','{\"network_started\":true}','{}','','','{}','2026-11-10T00:00:00Z')");
                    CHECK(d.f.store->live_attempt(request.attempt_id).at("transport").at("network_started") == true);
                }
                auto raw = driver_empty_report();
                raw.as_object()["detail"] = std::string(4U * 1024U * 1024U, 'x');
                return raw;
            });
        CHECK(invocations == 1);
        CHECK(report.at("batches_started") == 1 && report.at("batches_completed") == 1);
        CHECK(report.at("attempts_total") == 1 && report.at("new_reservations_total") == 1);
        CHECK(report.at("report_complete") == false);
        CHECK(report.at("network_count_complete") == observed);
        if (observed) {
            CHECK(report.at("network_started_calls") == 1);
            CHECK(report.at("provider_calls") == 1);
        } else {
            CHECK(report.at("network_started_calls").is_null());
            CHECK(report.at("provider_calls").is_null());
            CHECK(report.at("network_started_calls_lower_bound") == 0);
        }
        const auto& summary = report.at("batch_summaries").as_array().front();
        CHECK(summary.at("raw_batch_report_oversize") == true);
        CHECK(summary.at("new_attempts") == 1 && summary.at("attempt_set_sha256").is_string());
    }
}

void driver_mixed_live_observations_preserve_known_send_lower_bound() {
    for (const auto* partial : {"missing", "null", "wrong_type"}) {
        DriverFixture d(true);
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, false, Object{},
            [&](std::int64_t) {
                ++invocations;
                const auto known = d.f.request("mixed-known-send"), unknown = d.f.request("mixed-unknown-send");
                (void)d.f.reserve(known); (void)d.f.reserve(unknown);
                const auto known_claim = d.f.claim(known), unknown_claim = d.f.claim(unknown);
                const auto observe = [&](const ModelRequest& request, const Json& claim, const Json& transport) {
                    live_fixture::sql(d.f.path, "INSERT INTO live_transport_observations VALUES ('" + request.attempt_id +
                        "','" + live_fixture::text(claim, "claim_id") + "','" + sha256_hex("mixed synthetic transport " + request.attempt_id) +
                        "','" + canonical_json(transport) + "','{}','','','{}','2026-11-10T00:00:00Z')");
                };
                observe(known, known_claim, Object{{"network_started", true}});
                if (std::string_view(partial) == "null") observe(unknown, unknown_claim, Object{{"network_started", nullptr}});
                if (std::string_view(partial) == "wrong_type") observe(unknown, unknown_claim, Object{{"network_started", "true"}});
                CHECK(d.f.store->live_attempt(known.attempt_id).at("transport").at("network_started") == true);
                auto raw = driver_empty_report("worker_limit");
                raw.at("calls") = Array{Object{{"role", "dispatcher"}, {"attempt_id", known.attempt_id}},
                    Object{{"role", "dispatcher"}, {"attempt_id", unknown.attempt_id}}};
                raw.at("manager_calls") = 2;
                raw.at("network_started_calls") = nullptr;
                raw.at("network_count_complete") = false;
                return raw;
            });
        live_fixture::controls_regression_evidence(std::string("network-driver-") + partial,
            Object{{"report", report}, {"callbacks", invocations}}, d.f.path);
        CHECK(invocations == 1 && report.at("attempts_total") == 2);
        CHECK(report.at("network_count_complete") == false);
        CHECK(report.at("network_started_calls").is_null() && report.at("provider_calls").is_null());
        CHECK(report.at("network_started_calls_lower_bound") == 1);
        CHECK(report.at("batch_summaries").as_array().front().at("continued") == false);
    }
}

void driver_malformed_counts_and_unserializable_report_stop() {
    for (int fault = 0; fault != 7; ++fault) {
        DriverFixture d;
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
            [&](std::int64_t) {
                ++invocations;
                auto raw = driver_empty_report("worker_limit");
                if (fault == 0) raw.as_object().erase("calls");
                if (fault == 1) raw.as_object()["calls"] = "wrong type";
                if (fault == 2) raw.as_object()["manager_calls"] = -1;
                if (fault == 3) raw.as_object()["manager_calls"] = std::numeric_limits<std::uint64_t>::max();
                if (fault == 4) raw.as_object()["calls"] = Array{Object{{"role", "invented"}}};
                if (fault == 5) raw.as_object()["calls"] = Array{Object{{"role", "worker"}, {"attempt_id", "absent-attempt"}}};
                if (fault == 6) raw.as_object()["detail"] = std::string("\xc0\xaf", 2);
                return raw;
            });
        CHECK(invocations == 1);
        CHECK(report.at("report_complete") == false);
        CHECK(report.at("attempts_total") == 0);
        CHECK(report.at("model_milestone") == false && report.at("business_delivered") == false);
        CHECK(report.at("batch_summaries").as_array().size() == 1);
        CHECK(report.at("batch_summaries").as_array().front().at("continued") == false);
    }
}

void driver_strong_primary_and_every_blocked_reason() {
    for (const auto* primary : {"transport", "permission", "unknown_original_reason"}) {
        DriverFixture d;
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
            [&](std::int64_t) {
                ++invocations;
                auto raw = driver_empty_report(primary);
                raw.as_object()["blocked"] = Array{Object{{"role", "worker"}, {"node_id", "alpha"}, {"code", "correction_limit"}}};
                raw.as_object()["model_milestone"] = true;
                raw.as_object()["business_delivered"] = true;
                return raw;
            });
        CHECK(invocations == 1);
        CHECK(report.at("primary_stop") == (std::string(primary) == "unknown_original_reason" ? "unclassified" : primary));
        CHECK(driver_contains(report.at("raw_stop_codes"), primary));
        CHECK(driver_contains(report.at("raw_stop_codes"), "correction_limit"));
        CHECK(driver_contains(report.at("stop_classes"), "correction_limit"));
        CHECK(report.at("model_milestone") == false && report.at("business_delivered") == false);
    }
    DriverFixture d;
    auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
        [&](std::int64_t) {
            auto raw = driver_empty_report();
            raw.as_object()["blocked"] = Array{Object{{"code", "unknown_blocked_reason"}}};
            return raw;
        });
    CHECK(report.at("primary_stop") == "unclassified");
    CHECK(driver_contains(report.at("raw_stop_codes"), "unknown_blocked_reason"));
}

struct OutputBuffer : std::stringbuf {
    bool fail_write = false, fail_flush = false;
    std::streamsize xsputn(const char* bytes, std::streamsize size) override {
        return fail_write ? 0 : std::stringbuf::xsputn(bytes, size);
    }
    int_type overflow(int_type byte = traits_type::eof()) override {
        return fail_write ? traits_type::eof() : std::stringbuf::overflow(byte);
    }
    int sync() override { return fail_flush ? -1 : 0; }
};
struct RedirectOutput {
    std::streambuf* original;
    std::ios::iostate state = std::cout.rdstate(), exceptions = std::cout.exceptions();
    explicit RedirectOutput(std::streambuf* replacement) : original(std::cout.rdbuf()) {
        std::cout.exceptions(std::ios::goodbit);
        std::cout.clear();
        std::cout.rdbuf(replacement);
    }
    ~RedirectOutput() {
        std::cout.exceptions(std::ios::goodbit);
        std::cout.clear();
        std::cout.rdbuf(original);
        std::cout.clear(state);
        std::cout.exceptions(exceptions);
    }
};
std::string emitted_report(const Json& report) {
    OutputBuffer buffer;
    { RedirectOutput redirect(&buffer); output(report); }
    return buffer.str();
}

void driver_final_summary_bound_after_incomplete_totals() {
    const auto raw_report = [](std::size_t text_bytes) {
        auto raw = driver_empty_report(std::string(text_bytes, 'u'));
        raw.at("manager_calls") = -1;  // real final-incomplete path, no callback effects
        return raw;
    };
    // Calibrate from an actual small driver result. Removing exactly the
    // known final marker measures serialization overhead without copying a
    // production capacity/stop decision. Both raw byte counts have four
    // digits, and every variable-length digest has the same fixed width.
    DriverFixture calibration;
    std::int64_t calibration_calls = 0;
    const auto small_raw = raw_report(4096);
    const auto small = controls_run_driver(*calibration.f.store, calibration.f.request_id, 2, true, Object{},
        [&](std::int64_t) { ++calibration_calls; return small_raw; });
    CHECK(calibration_calls == 1 && small.at("report_complete") == false);
    auto unmarked = small.at("batch_summaries").as_array().front();
    CHECK(unmarked.at("stopped_after_incomplete_totals") == true);
    CHECK(unmarked.as_object().erase("stopped_after_incomplete_totals") == 1);
    const auto measured_overhead = canonical_json(unmarked).size();
    CHECK(measured_overhead < 8191);
    const auto exact_text_bytes = std::size_t{4096} + 8191 - measured_overhead;
    CHECK(exact_text_bytes > 4096 && exact_text_bytes < 8192);
    live_fixture::controls_regression_evidence("summary-calibration", Object{{"actual_report", small},
        {"unmarked_summary_bytes", measured_overhead}, {"exact_unknown_text_bytes", exact_text_bytes}}, calibration.f.path);

    bool every_final_summary_bounded = true;
    for (const std::size_t extra : {std::size_t{0}, std::size_t{1}}) {
        DriverFixture d;
        std::int64_t invocations = 0;
        const auto raw = raw_report(exact_text_bytes + extra);
        const auto raw_bytes = canonical_json(raw);
        CHECK(raw_bytes.size() >= 1000 && raw_bytes.size() <= 9999);
        const auto before = d.f.store->budget(d.f.request_id);
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
            [&](std::int64_t) { ++invocations; return raw; });
        const auto& summary = report.at("batch_summaries").as_array().front();
        const auto emitted = emitted_report(report);
        const auto label = extra == 0 ? "summary-exact" : "summary-fallback";
        live_fixture::controls_regression_evidence(std::string(label) + "-report", report, d.f.path);
        live_fixture::controls_regression_evidence(std::string(label) + "-facts", Object{{"raw_report", raw},
            {"raw_sha256", sha256_hex(raw_bytes)}, {"raw_bytes", raw_bytes.size()},
            {"summary_bytes_with_newline", canonical_json(summary).size() + 1},
            {"whole_emitted_bytes", emitted.size()}, {"callbacks", invocations}, {"budget_before", before},
            {"budget_after", d.f.store->budget(d.f.request_id)},
            {"predicted_unmarked_summary_bytes", 8191 + extra}});
        CHECK(invocations == 1);
        CHECK(report.at("request_id") == d.f.request_id);
        CHECK(report.at("batches_started") == 1 && report.at("batches_completed") == 1);
        CHECK(report.at("report_complete") == false && report.at("attempts_total") == 0);
        CHECK(report.at("batch_summaries").as_array().size() == 1);
        CHECK(summary.at("index") == 1 && summary.at("completed") == true);
        CHECK(summary.at("continued") == false);
        CHECK(summary.at("report_sha256") == sha256_hex(raw_bytes));
        CHECK(summary.at("report_bytes") == raw_bytes.size());
        CHECK(summary.at("new_attempts") == 0);
        CHECK(summary.at("attempt_set_sha256") == sha256_hex(canonical_json(Array{})));
        CHECK(summary.at("event_seq_first").is_int64() && summary.at("event_seq_last").is_int64());
        CHECK(driver_contains(report.at("raw_stop_codes"), std::string(exact_text_bytes + extra, 'u').c_str()));
        CHECK(d.f.store->budget(d.f.request_id) == before);
        if (extra == 0 && summary.as_object().if_contains("summary_truncated") == nullptr) {
            auto before_final_marker = summary;
            CHECK(before_final_marker.as_object().erase("stopped_after_incomplete_totals") == 1);
            CHECK(canonical_json(before_final_marker).size() == 8191);
        }
        if (extra == 1) CHECK(summary.at("summary_truncated") == true);
        every_final_summary_bounded = every_final_summary_bounded && canonical_json(summary).size() + 1 <= 8192;
        auto terminal = report;
        terminal.as_object().erase("batch_summaries");
        CHECK(canonical_json(terminal).size() + 1 <= 65536);
        CHECK(emitted.size() <= 589824 && emitted.back() == '\n');
        CHECK(parse_json_strict(emitted, 589824) == report);
    }
    CHECK(every_final_summary_bounded);
}

void driver_capacity_uses_actual_emitted_framing() {
    for (const std::size_t raw_code_bytes : {std::size_t{8192}, std::size_t{65536}}) {
        DriverFixture d;
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 64, true,
            Object{{"script_source_sha256", std::string(65536, 's')}},
            [&](std::int64_t) {
                ++invocations;
                auto raw = driver_empty_report();
                raw.as_object()["blocked"] = Array{Object{{"code", std::string(raw_code_bytes, 'z')}}};
                return raw;
            });
        CHECK(invocations == 1);
        CHECK(report.at("report_complete") == false);
        CHECK(report.at("batch_summaries").as_array().size() == 1);
        CHECK(report.at("identity_fields_sha256").is_string());
        CHECK(report.at("batch_identity_sha256").is_string());
        const auto& summary = report.at("batch_summaries").as_array().front();
        CHECK(summary.at("report_sha256").is_string() && summary.at("attempt_set_sha256").is_string());
        CHECK(canonical_json(summary).size() + 1 <= 8192);
        auto terminal = report;
        terminal.as_object().erase("batch_summaries");
        CHECK(canonical_json(terminal).size() + 1 <= 65536);
        const auto emitted = emitted_report(report);
        CHECK(emitted.size() <= 589824 && emitted.back() == '\n');
        CHECK(parse_json_strict(emitted, 589824) == report);
    }
    for (const std::int64_t limit : {0, 65}) {
        DriverFixture d;
        std::int64_t invocations = 0;
        ::test::throws<Error>([&] {
            (void)controls_run_driver(*d.f.store, d.f.request_id, limit, true, Object{},
                [&](std::int64_t) { ++invocations; return driver_empty_report(); });
        });
        CHECK(invocations == 0 && live_fixture::number(d.f.store->budget(d.f.request_id), "attempts") == 0);
    }
    // The common production output function adds exactly one framing newline.
    for (const std::size_t boundary : {std::size_t{8192}, std::size_t{65536}, std::size_t{589824}}) {
        const auto overhead = canonical_json(Object{{"payload", ""}}).size() + 1;
        const auto exact = emitted_report(Object{{"payload", std::string(boundary - overhead, 'x')}});
        const auto over = emitted_report(Object{{"payload", std::string(boundary - overhead + 1, 'x')}});
        CHECK(exact.size() == boundary && over.size() == boundary + 1);
    }
}

void production_output_write_and_flush_fail_nonzero() {
    for (const bool flush_failure : {false, true}) {
        OutputBuffer buffer;
        buffer.fail_flush = flush_failure;
        buffer.fail_write = !flush_failure;
        {
            RedirectOutput redirect(&buffer);
            bool caught = false;
            try { output(Object{{"retained_effect", "original"}}); }
            catch (const Error& failure) {
                caught = true;
                CHECK(failure.code() == ErrorCode::Storage);
                CHECK(std::string(failure.what()).find("delivery_of_report_failed") != std::string::npos);
            }
            CHECK(caught);
        }
        OutputBuffer main_buffer;
        main_buffer.fail_flush = flush_failure;
        main_buffer.fail_write = !flush_failure;
        char program[] = "wls", command[] = "resources", option[] = "--source-root";
        std::string source_path = WLS_SOURCE_DIR;
        char* argv[] = {program, command, option, source_path.data()};
        int status = 0;
        { RedirectOutput redirect(&main_buffer); status = controls_embedded_cli_main(4, argv); }
        CHECK(status != 0);
    }
}

void driver_present_corruption_and_normal_active_tail() {
    {
        DriverFixture d;
        live_fixture::sql(d.f.path, "DELETE FROM events WHERE type='controls_admission' AND request_id='" + d.f.request_id + "'");
        CHECK(d.f.store->controls_business_gate(d.f.request_id).at("status") == "legacy");
        std::int64_t invocations = 0;
        bool refused = false;
        try {
            (void)controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
                [&](std::int64_t) { ++invocations; return driver_empty_report(); });
        } catch (const Error& failure) {
            refused = true;
            CHECK(failure.code() == ErrorCode::InvalidInput);
            CHECK(std::string(failure.what()).find("controls_legacy_unavailable") != std::string::npos);
        }
        CHECK(refused && invocations == 0);
    }
    {
        DriverFixture d;
        live_fixture::sql(d.f.path, "UPDATE events SET result='null' WHERE request_id='" + d.f.request_id + "' AND type='controls_admission'");
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
            [&](std::int64_t) { ++invocations; return driver_empty_report(); });
        CHECK(invocations == 0 && report.at("batches_started") == 0);
        CHECK(driver_contains(report.at("raw_stop_codes"), "controls_marker_corrupt"));
    }
    {
        DriverFixture d;
        const auto request = d.f.request("normal-active-original");
        (void)d.f.reserve(request);
        CHECK(d.f.store->controls_business_gate(d.f.request_id).at("status") == "open");
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.f.store, d.f.request_id, 2, true, Object{},
            [&](std::int64_t) { ++invocations; return driver_empty_report(); });
        CHECK(invocations == 0 && report.at("history_complete") == false);
        CHECK(driver_contains(report.at("raw_stop_codes"), "incomplete_tail"));
        CHECK(d.f.store->attempt(request.attempt_id).at("status") == "PENDING");
    }
}

struct DriverWorkflow {
    DriverFixture d;
    SchemaRegistry schemas{d.assets.source / "schemas"};
    ::test::TempDir workspace;
    std::unique_ptr<WorkerTools> tools;
    std::unique_ptr<EventRuntime> runtime;
    EventFakeScript full;
    std::string task_id;
    DriverWorkflow() {
        auto input = live_fixture::admission(d.assets, d.f.live, "driver-real-worker", false, true);
        input.original_text = live_fixture::read(d.assets.source / "tests/fixtures/s13_request.txt");
        input.constraint_catalog = parse_json_strict(live_fixture::read(d.assets.source / "tests/fixtures/s13_constraints.json"));
        const auto frozen = FrozenRequestConfig::restore(canonical_json(input.config_snapshot), sha256_hex(canonical_json(input.config_snapshot)));
        const auto grants = frozen.worker_tools();
        input.tool_allowlist.assign(grants.begin(), grants.end());
        d.f.request_id = live_fixture::text(d.f.store->admit(input), "request_id");
        tools = std::make_unique<WorkerTools>(*d.f.store, workspace.path);
        runtime = std::make_unique<EventRuntime>(*d.f.store, schemas, std::map<std::string, WorkerTools*>{{"alpha", tools.get()}});
        full = event_fake_script_from_json(parse_json_strict(live_fixture::read(d.assets.source / "tests/fixtures/s15_fake_complete.json")));
        auto setup = full;
        setup.default_steps.resize(1);
        setup.by_node.at("alpha").resize(1);
        CHECK(runtime->pump(d.f.request_id, setup).at("calls").as_array().size() == 2);
        task_id = live_fixture::text(d.f.store->tasks(d.f.request_id).front(), "task_id");
    }
    EventFakeScript writers(std::int64_t batch = 1) const {
        EventFakeScript script;
        auto first = full.by_node.at("alpha").at(1);
        first.tool_calls.front().arguments.as_object()["path"] = "driver-effect-" + std::to_string(batch) + ".txt";
        first.tool_calls.front().arguments.as_object()["content"] = "retained actual batch effect " + std::to_string(batch);
        auto second = first;
        second.role_call_index = 2;
        second.tool_calls.front().call_id += "-next";
        script.by_node["alpha"] = {first, second};
        return script;
    }
};

void driver_real_worker_cap_and_running_target_continue() {
    DriverWorkflow d;
    const auto next = d.writers();
    std::int64_t invocations = 0;
    const auto before = live_fixture::number(d.d.f.store->budget(d.d.f.request_id), "attempts");
    const auto report = controls_run_driver(*d.d.f.store, d.d.f.request_id, 2, true, Object{},
        [&](std::int64_t index) {
            ++invocations;
            auto raw = d.runtime->pump(d.d.f.request_id, d.writers(index));
            if (index == 2) raw.as_object()["stop_reason"] = "transport";
            return raw;
        },
        [&](const Json& entry, std::int64_t) { return d.runtime->controls_next_target(d.d.f.request_id, entry, &next); });
    CHECK(invocations == 2 && report.at("batches_started") == 2 && report.at("batches_completed") == 2);
    CHECK(report.at("attempts_total") == 2 && report.at("new_reservations_total") == 2);
    CHECK(live_fixture::number(d.d.f.store->budget(d.d.f.request_id), "attempts") == before + 2);
    CHECK(report.at("history_complete") == true && report.at("counts_complete") == true);
    const auto& first = report.at("batch_summaries").as_array().front();
    CHECK(first.at("semantic_progress") == true && first.at("continued") == true);
    CHECK(first.at("worker_calls") == 1 && first.at("manager_calls") == 0);
    CHECK(first.at("continuation_target").at("task_id") == d.task_id);
    CHECK(first.at("continuation_target").at("role") == "worker");
    CHECK(d.d.f.store->task(d.task_id).at("status") == "RUNNING");
    CHECK(live_fixture::read(d.workspace.path / "driver-effect-1.txt") == "retained actual batch effect 1");
    CHECK(live_fixture::read(d.workspace.path / "driver-effect-2.txt") == "retained actual batch effect 2");
}

void driver_real_effect_survives_serializer_and_output_failure() {
    for (const bool serializer_failure : {false, true}) {
        DriverWorkflow d;
        std::int64_t invocations = 0;
        const auto attempts = live_fixture::number(d.d.f.store->budget(d.d.f.request_id), "attempts");
        const auto report = controls_run_driver(*d.d.f.store, d.d.f.request_id, 2, true, Object{},
            [&](std::int64_t index) {
                ++invocations;
                auto raw = d.runtime->pump(d.d.f.request_id, d.writers(index));
                raw.at("stop_reason") = "transport";
                if (serializer_failure) raw.as_object()["detail"] = std::string("\xc0\xaf", 2);
                return raw;
            });
        CHECK(invocations == 1 && report.at("attempts_total") == 1);
        CHECK(live_fixture::number(d.d.f.store->budget(d.d.f.request_id), "attempts") == attempts + 1);
        CHECK(live_fixture::read(d.workspace.path / "driver-effect-1.txt") == "retained actual batch effect 1");
        CHECK(!std::filesystem::exists(d.workspace.path / "driver-effect-2.txt"));
        if (serializer_failure) {
            CHECK(report.at("report_complete") == false);
            CHECK(report.at("batch_summaries").as_array().front().at("report_sha256").is_null());
        }
        for (const bool flush : {false, true}) {
            OutputBuffer buffer;
            buffer.fail_write = !flush; buffer.fail_flush = flush;
            bool failed = false;
            {
                RedirectOutput redirect(&buffer);
                try { output(report); }
                catch (const Error& failure) {
                    failed = true;
                    CHECK(failure.code() == ErrorCode::Storage);
                    CHECK(std::string(failure.what()).find("delivery_of_report_failed") != std::string::npos);
                }
            }
            CHECK(failed && invocations == 1);
            CHECK(live_fixture::number(d.d.f.store->budget(d.d.f.request_id), "attempts") == attempts + 1);
            CHECK(live_fixture::read(d.workspace.path / "driver-effect-1.txt") == "retained actual batch effect 1");
        }
    }
}

void driver_actual_cap_rejects_each_mismatched_target() {
    for (const auto* field : {"request_id", "role", "task_id", "node_id", "config_sha256", "source_sha256", "target_sha256", "task_state_version"}) {
        DriverWorkflow d;
        const auto next = d.writers();
        std::int64_t invocations = 0, altered = 0;
        const auto report = controls_run_driver(*d.d.f.store, d.d.f.request_id, 2, true, Object{},
            [&](std::int64_t index) {
                ++invocations;
                auto raw = d.runtime->pump(d.d.f.request_id, d.writers(index));
                for (auto& entry : raw.at("blocked").as_array()) {
                    if (entry.at("code") != "worker_limit") continue;
                    CHECK(entry.at("controls_target").is_object());
                    auto& target = entry.at("controls_target").as_object();
                    if (std::string(field) == "task_state_version") target[field] = target.at(field).as_int64() + 1;
                    else target[field] = "wrong-original-identity";
                    ++altered;
                }
                return raw;
            },
            [&](const Json& entry, std::int64_t) { return d.runtime->controls_next_target(d.d.f.request_id, entry, &next); });
        CHECK(altered == 1 && invocations == 1 && report.at("batches_started") == 1);
        CHECK(report.at("attempts_total") == 1);
        const auto& summary = report.at("batch_summaries").as_array().front();
        CHECK(summary.at("semantic_progress") == true && summary.at("continued") == false);
        CHECK(live_fixture::read(d.workspace.path / "driver-effect-1.txt") == "retained actual batch effect 1");
        CHECK(!std::filesystem::exists(d.workspace.path / "driver-effect-2.txt"));
    }
}

void actual_pending_targets_require_current_original_source() {
    for (const bool acceptance : {false, true}) {
        DriverWorkflow d;
        auto submit = d.full;
        submit.default_steps.clear();
        submit.by_node.at("alpha").erase(submit.by_node.at("alpha").begin());
        // Keep write+submit, optionally the TeamLead's captured check.
        submit.by_node.at("alpha").resize(acceptance ? 3 : 2);
        if (acceptance) submit.by_node.at("alpha").back().role_call_index = 1;
        EventPumpOptions limits;
        limits.max_worker_calls_per_task = 2;
        const auto raw = d.runtime->pump(d.d.f.request_id, submit, limits);
        const auto expected = acceptance ? "acceptance_pending" : "submission_pending";
        bool found = false;
        for (const auto& entry : raw.at("blocked").as_array()) {
            if (entry.at("code") != expected) continue;
            found = true;
            EventFakeScript next;
            auto step = acceptance ? d.full.default_steps.at(1) : d.full.by_node.at("alpha").at(3);
            step.role_call_index = 1;
            if (acceptance) next.default_steps = {step};
            else next.by_node["alpha"] = {step};
            const auto target = d.runtime->controls_next_target(d.d.f.request_id, entry, &next);
            CHECK(target.is_object() && target == entry.at("controls_target"));
            CHECK(target.at("task_id") == d.task_id);
            CHECK(target.at("role") == (acceptance ? "dispatcher" : "team_lead"));
            auto foreign = entry;
            foreign.at("task_id") = "task-not-original";
            CHECK(d.runtime->controls_next_target(d.d.f.request_id, foreign, &next).is_null());
            EventFakeScript missing_script;
            CHECK(d.runtime->controls_next_target(d.d.f.request_id, entry, &missing_script).is_null());
        }
        CHECK(found);
    }
}

std::string driver_sql_quote(std::string_view value) {
    std::string encoded = "'";
    for (const char byte : value) { encoded.push_back(byte); if (byte == '\'') encoded.push_back(byte); }
    return encoded + "'";
}
void complete_driver_workflow(DriverWorkflow& d) {
    auto script = d.full;
    script.default_steps.erase(script.default_steps.begin());
    script.default_steps.front().role_call_index = 1;
    auto& steps = script.by_node.at("alpha");
    steps.erase(steps.begin());
    for (auto& step : steps)
        if (step.role == Role::TeamLead) --step.role_call_index;
    EventPumpOptions limits;
    limits.max_worker_calls_per_task = 2;
    const auto report = d.runtime->pump(d.d.f.request_id, script, limits);
    CHECK(report.at("calls").as_array().size() == 5);
    CHECK(d.d.f.store->task(d.task_id).at("status") == "ACCEPTED");
    CHECK(d.d.f.store->experience_jobs(d.d.f.request_id).front().at("status") == "DONE");
    CHECK(d.d.f.store->controls_model_milestone(d.d.f.request_id).at("milestone") == true);
    CHECK(d.d.f.store->request(d.d.f.request_id).at("status") == "RUNNING");
}

void driver_strong_stop_preserves_genuine_milestone_and_original_effect() {
    for (const auto* reason : {"transport", "deadline", "correction_limit", "unmapped-original-stop"}) {
        DriverWorkflow d;
        complete_driver_workflow(d);
        const auto original_effect = live_fixture::read(d.workspace.path / "result.txt");
        const auto before = d.d.f.store->budget(d.d.f.request_id);
        std::int64_t invocations = 0;
        const auto report = controls_run_driver(*d.d.f.store, d.d.f.request_id, 2, true, Object{},
            [&](std::int64_t) { ++invocations; return driver_empty_report(reason); });
        CHECK(invocations == 1 && report.at("batches_started") == 1);
        CHECK(report.at("model_milestone") == true && report.at("business_delivered") == false);
        CHECK(report.at("primary_stop") == (std::string_view(reason) == "unmapped-original-stop" ? "unclassified" : reason));
        CHECK(driver_contains(report.at("raw_stop_codes"), reason));
        CHECK(report.at("batch_summaries").as_array().front().at("continued") == false);
        CHECK(d.d.f.store->budget(d.d.f.request_id) == before);
        CHECK(live_fixture::read(d.workspace.path / "result.txt") == original_effect);
    }
}
bool milestone_is_verified(DriverWorkflow& d) {
    try { return d.d.f.store->controls_model_milestone(d.d.f.request_id).at("milestone") == true; }
    catch (const Error&) { return false; }
}
void current_milestone_requires_actual_source_and_complete_report() {
    for (const auto* fault : {"none", "missing_report", "wrong_report_ref", "wrong_job_team",
            "wrong_acceptance", "wrong_submission", "wrong_source_record", "stale_revisions", "status_spoof"}) {
        DriverWorkflow d;
        complete_driver_workflow(d);
        const auto job = d.d.f.store->experience_jobs(d.d.f.request_id).front();
        const auto id = live_fixture::text(job, "experience_id");
        if (std::string(fault) == "missing_report")
            live_fixture::sql(d.d.f.path, "DELETE FROM experience_reports WHERE experience_id=" + driver_sql_quote(id));
        if (std::string(fault) == "wrong_report_ref")
            live_fixture::sql(d.d.f.path, "UPDATE experience_jobs SET report_ref='rep-wrong-original',state_version=state_version+1 WHERE experience_id=" + driver_sql_quote(id));
        if (std::string(fault) == "wrong_job_team" || std::string(fault).starts_with("wrong_a") ||
                std::string(fault) == "wrong_submission" || std::string(fault) == "wrong_source_record") {
            auto source = job.at("source_snapshot");
            if (std::string(fault) == "wrong_acceptance") source.at("acceptance").at("acceptance_id") = "acc-foreign";
            if (std::string(fault) == "wrong_submission") source.at("submission").at("submission_id") = "sub-foreign";
            if (std::string(fault) == "wrong_source_record") source.at("submission").at("record_sha256") = std::string(64, '0');
            const auto team = std::string(fault) == "wrong_job_team" ? "team-foreign" : live_fixture::text(job, "team_id");
            live_fixture::sql(d.d.f.path, "INSERT OR REPLACE INTO experience_jobs SELECT experience_id,request_id,task_id," +
                driver_sql_quote(team) + ",based_on_requirements_revision,plan_revision,task_revision,instruction_revision,status,report_ref," +
                driver_sql_quote(canonical_json(source)) + ",claimed_by,lease_epoch,attempts,state_version,created_at,updated_at FROM experience_jobs WHERE experience_id=" + driver_sql_quote(id));
            CHECK(live_fixture::sql_text(d.d.f.path, "SELECT source_snapshot FROM experience_jobs WHERE experience_id=" + driver_sql_quote(id)) == canonical_json(source));
        }
        if (std::string(fault) == "stale_revisions")
            live_fixture::sql(d.d.f.path, "UPDATE requests SET requirements_revision=requirements_revision+1,state_version=state_version+1 WHERE request_id=" + driver_sql_quote(d.d.f.request_id));
        if (std::string(fault) == "status_spoof")
            live_fixture::sql(d.d.f.path, "DELETE FROM candidate_acceptances WHERE task_id=" + driver_sql_quote(d.task_id));
        CHECK(milestone_is_verified(d) == (std::string(fault) == "none"));
    }
}

struct DriverPublishedReceipt {
    ::test::TempDir directory;
    std::unique_ptr<DeliveryDirectory> output_root{std::make_unique<DeliveryDirectory>(directory.path)};
    Json receipt, prepared, publication;
    explicit DriverPublishedReceipt(DriverWorkflow& d) {
        complete_driver_workflow(d);
        auto& store = *d.d.f.store;
        const auto root = store.host_actor(Role::Root, d.d.f.request_id);
        const auto binding = store.request_binding(d.d.f.request_id);
        prepared = store.prepare_delivery(root, binding, live_fixture::number(store.request(d.d.f.request_id), "state_version"),
            output_root->root_identity(), "prepare-real-controls-receipt");
        const auto published = output_root->publish(prepared.at("frozen_plan"));
        CHECK(published.status == DeliveryMutationStatus::Succeeded);
        publication = published.publication;
        const auto delivery_id = live_fixture::text(prepared, "delivery_id");
        (void)store.record_delivery_publication(root, binding, delivery_id, live_fixture::number(prepared, "state_version"),
            publication, "record-real-controls-publication");
        const auto acknowledged = store.acknowledge_delivery(root, binding, delivery_id, live_fixture::text(prepared, "manifest_sha256"),
            binding.requirements_revision, binding.plan_revision, "test-recipient", "durable_local", "controls-exact-receipt", publication);
        receipt = acknowledged.at("receipt");
        CHECK(store.controls_delivery_receipt_facts(d.d.f.request_id).at("verified") == true);
    }
    ~DriverPublishedReceipt() {
        output_root.reset();
        // Unseal only this fixture's own retained package for TempDir teardown.
        std::error_code ignored;
        for (const auto& item : std::filesystem::recursive_directory_iterator(directory.path, ignored))
            std::filesystem::permissions(item.path(), std::filesystem::perms::owner_all,
                std::filesystem::perm_options::add, ignored);
    }
};
bool receipt_is_verified(DriverWorkflow& d) {
    try { return d.d.f.store->controls_delivery_receipt_facts(d.d.f.request_id).at("verified") == true; }
    catch (const Error&) { return false; }
}
void current_receipt_rejects_each_wrong_original_identity() {
    for (const auto* field : {"none", "request_id", "receipt_id", "root_id", "idempotency_key", "captured_binding",
            "publication", "missing_ack", "ack_publication", "stale_pair"}) {
        DriverWorkflow d;
        DriverPublishedReceipt published(d);
        auto body = published.receipt;
        const auto receipt_id = live_fixture::text(body, "receipt_id");
        if (std::string(field) == "missing_ack")
            live_fixture::sql(d.d.f.path, "DELETE FROM events WHERE command_id=" + driver_sql_quote("receipt:" + receipt_id));
        else if (std::string(field) == "ack_publication") {
            const auto condition = " WHERE command_id=" + driver_sql_quote("receipt:" + receipt_id);
            auto command = parse_json_strict(live_fixture::sql_text(d.d.f.path, "SELECT payload FROM events" + condition));
            command.at("publication").at("verified") = false;
            live_fixture::sql(d.d.f.path, "UPDATE events SET payload=" + driver_sql_quote(canonical_json(command)) +
                ",command_sha256=" + driver_sql_quote(sha256_hex(canonical_json(command))) + condition);
        } else if (std::string(field) == "stale_pair") {
            live_fixture::sql(d.d.f.path, "UPDATE requests SET requirements_revision=requirements_revision+1,state_version=state_version+1 WHERE request_id=" +
                driver_sql_quote(d.d.f.request_id));
        } else if (std::string(field) != "none") {
            if (std::string(field) == "captured_binding") body.at(field).at("requirements_revision") = 2;
            else if (std::string(field) == "publication") body.at(field).at("verified") = false;
            else body.at(field) = "wrong-original-identity";
            Object intent;
            for (const auto* key : {"request_id", "root_id", "delivery_id", "manifest_sha256", "requirements_revision",
                    "plan_revision", "recipient", "method", "idempotency_key"}) intent[key] = body.at(key);
            // The replacement record carries a self-consistent nine-field
            // command hash. Further original identity checks must still fail.
            live_fixture::sql(d.d.f.path, "INSERT OR REPLACE INTO delivery_receipts SELECT receipt_id,request_id,delivery_id,root_id,idempotency_key," +
                driver_sql_quote(sha256_hex(canonical_json(intent))) + "," + driver_sql_quote(canonical_json(body)) +
                ",result,created_at FROM delivery_receipts WHERE receipt_id=" + driver_sql_quote(receipt_id));
            CHECK(live_fixture::sql_text(d.d.f.path, "SELECT body FROM delivery_receipts WHERE receipt_id=" + driver_sql_quote(receipt_id)) == canonical_json(body));
        }
        CHECK(receipt_is_verified(d) == (std::string(field) == "none"));
    }
}

void current_receipt_rejects_self_consistently_rehashed_wrong_semantics() {
    for (const auto* fault : {"empty_recipient", "unknown_method", "wrong_manifest", "foreign_request", "wrong_binding"}) {
        DriverWorkflow d;
        DriverPublishedReceipt published(d);
        auto body = published.receipt;
        const auto receipt_id = live_fixture::text(body, "receipt_id");
        const auto ack_condition = " WHERE command_id=" + driver_sql_quote("receipt:" + receipt_id);
        auto ack = parse_json_strict(live_fixture::sql_text(d.d.f.path, "SELECT payload FROM events" + ack_condition));
        auto ack_result = parse_json_strict(live_fixture::sql_text(d.d.f.path, "SELECT result FROM events" + ack_condition));
        if (std::string_view(fault) == "empty_recipient") body.at("recipient") = "";
        if (std::string_view(fault) == "unknown_method") body.at("method") = "invented_delivery_method";
        if (std::string_view(fault) == "wrong_manifest") body.at("manifest_sha256") = std::string(64, '0');
        if (std::string_view(fault) == "foreign_request") body.at("request_id") = "req-foreign-original";
        if (std::string_view(fault) == "wrong_binding") body.at("captured_binding").at("requirements_revision") = 2;
        for (const auto& field : body.as_object()) ack.as_object()[field.key()] = field.value();
        ack_result.at("receipt") = body;
        Object intent;
        for (const auto* key : {"request_id", "root_id", "delivery_id", "manifest_sha256", "requirements_revision",
                "plan_revision", "recipient", "method", "idempotency_key"}) intent[key] = body.at(key);
        // The fingerprint uses the row's own original request/root/package.
        // Rehashing both receipt and ACK must not replace semantic proof.
        for (const auto* key : {"request_id", "root_id", "delivery_id"}) intent[key] = published.receipt.at(key);
        live_fixture::sql(d.d.f.path, "INSERT OR REPLACE INTO delivery_receipts SELECT receipt_id,request_id,delivery_id,root_id,idempotency_key," +
            driver_sql_quote(sha256_hex(canonical_json(intent))) + "," + driver_sql_quote(canonical_json(body)) + "," +
            driver_sql_quote(canonical_json(ack_result)) + ",created_at FROM delivery_receipts WHERE receipt_id=" + driver_sql_quote(receipt_id));
        live_fixture::sql(d.d.f.path, "UPDATE events SET payload=" + driver_sql_quote(canonical_json(ack)) +
            ",result=" + driver_sql_quote(canonical_json(ack_result)) + ",command_sha256=" +
            driver_sql_quote(sha256_hex(canonical_json(ack))) + ack_condition);
        CHECK(parse_json_strict(live_fixture::sql_text(d.d.f.path, "SELECT result FROM events" + ack_condition)).at("receipt") == body);
        CHECK(live_fixture::sql_text(d.d.f.path, "SELECT command_sha256 FROM events" + ack_condition) == sha256_hex(canonical_json(ack)));
        CHECK(live_fixture::sql_text(d.d.f.path, "SELECT command_sha256 FROM delivery_receipts WHERE receipt_id=" +
            driver_sql_quote(receipt_id)) == sha256_hex(canonical_json(intent)));
        CHECK(!receipt_is_verified(d));
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto regression = live_fixture::controls_regression_options(argc, argv, 1);
        if (!regression.empty()) {
            if (regression == "final_summary_bound")
                return ::test::run({{"final_summary_bound", driver_final_summary_bound_after_incomplete_totals}});
            if (regression == "partial_network_lower_bound") return ::test::run({
                {"partial_network_helper", run_totals_checked_boundaries},
                {"partial_network_driver", driver_mixed_live_observations_preserve_known_send_lower_bound}});
            throw std::runtime_error("unknown Controls regression selector");
        }
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
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
        {"streaming_canonical_digest_matches_reference", streaming_canonical_digest_matches_reference},
        {"run_totals_checked_boundaries", run_totals_checked_boundaries},
        {"streaming_program_created_utf8_and_depth_boundaries", streaming_program_created_utf8_and_depth_boundaries},
        {"streaming_aggregate_sorting_scratch_boundary", streaming_aggregate_sorting_scratch_boundary},
        {"driver_throw_observes_original_reservations", driver_throw_observes_original_reservations},
        {"driver_throw_interval_excludes_prerun_and_foreign_events", driver_throw_interval_excludes_prerun_and_foreign_events},
        {"driver_live_oversize_retains_network_or_unknown", driver_live_oversize_retains_network_or_unknown},
        {"driver_mixed_live_observations_preserve_known_send_lower_bound", driver_mixed_live_observations_preserve_known_send_lower_bound},
        {"driver_malformed_counts_and_unserializable_report_stop", driver_malformed_counts_and_unserializable_report_stop},
        {"driver_strong_primary_and_every_blocked_reason", driver_strong_primary_and_every_blocked_reason},
        {"driver_capacity_uses_actual_emitted_framing", driver_capacity_uses_actual_emitted_framing},
        {"driver_final_summary_bound_after_incomplete_totals", driver_final_summary_bound_after_incomplete_totals},
        {"production_output_write_and_flush_fail_nonzero", production_output_write_and_flush_fail_nonzero},
        {"driver_present_corruption_and_normal_active_tail", driver_present_corruption_and_normal_active_tail},
        {"driver_real_worker_cap_and_running_target_continue", driver_real_worker_cap_and_running_target_continue},
        {"driver_real_effect_survives_serializer_and_output_failure", driver_real_effect_survives_serializer_and_output_failure},
        {"driver_actual_cap_rejects_each_mismatched_target", driver_actual_cap_rejects_each_mismatched_target},
        {"actual_pending_targets_require_current_original_source", actual_pending_targets_require_current_original_source},
        {"current_milestone_requires_actual_source_and_complete_report", current_milestone_requires_actual_source_and_complete_report},
        {"driver_strong_stop_preserves_genuine_milestone_and_original_effect", driver_strong_stop_preserves_genuine_milestone_and_original_effect},
        {"current_receipt_rejects_each_wrong_original_identity", current_receipt_rejects_each_wrong_original_identity},
        {"current_receipt_rejects_self_consistently_rehashed_wrong_semantics", current_receipt_rejects_self_consistently_rehashed_wrong_semantics},
    };
    return ::test::run(cases);
}
