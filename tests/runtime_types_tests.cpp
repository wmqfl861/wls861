#include "wls/runtime_types.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace wls::s1;

void strict_json() {
    CHECK(parse_json_strict("  {\"中文\": [true, null, -2, 1.5]}\n").is_object());
    for (const std::string text : {"{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}",
         "[{\"b\":1,\"b\":2}]", "{\"a\":{\"x\":1,\"x\":2}}", "{}{}", "{} x", "[1,]", "//x\n1",
         "1e400", "NaN", "Infinity", "\"\\uD800\"", "", " ", "{\"unterminated\":1"})
        test::throws<Error>([&] { (void)parse_json_strict(text); });
    const std::string invalid_utf8 = std::string("\"") + static_cast<char>(0xc0) + static_cast<char>(0xaf) + "\"";
    test::throws<Error>([&] { (void)parse_json_strict(invalid_utf8); });
    CHECK(parse_json_strict(R"({"same":1,"nested":{"same":2},"last":{"same":3}})").is_object());
    test::throws<Error>([] { (void)parse_json_strict("[1,2]", 4); });
    test::throws<Error>([] { (void)parse_json_strict("[[[]]]", 100, 2); });
    CHECK(parse_json_strict("18446744073709551615").as_uint64() == std::numeric_limits<std::uint64_t>::max());
}

void canonical_and_digest() {
    const auto left = parse_json_strict(R"({"z":[3,2,1],"a":{"y":2,"x":1}})");
    const auto right = parse_json_strict(R"({"a":{"x":1,"y":2},"z":[3,2,1]})");
    CHECK(canonical_json(left) == canonical_json(right));
    CHECK(canonical_json(left) == R"({"a":{"x":1,"y":2},"z":[3,2,1]})");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(std::string("a\0b", 3)) != sha256_hex("a"));
    test::throws<Error>([] { (void)canonical_json(Json(std::numeric_limits<double>::infinity())); });
    test::throws<Error>([] { (void)canonical_json(Json(std::string(1, static_cast<char>(0xff)))); });
}

void canonical_strings_keep_original_boost_bytes_and_utf8_guards() {
    std::string ascii;
    for (unsigned int byte = 0; byte < 128; ++byte) ascii.push_back(static_cast<char>(byte));
    const std::vector<std::string> strings{
        "", ascii, "\"\\\b\f\n\r\t/", "中文😀\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf",
        "early\n" + std::string(900000, 'p'), std::string(32768, '\x01'),
        std::string(32768, '\\') + "\nend", std::string("nul\0tail", 8)};
    for (const auto& text : strings) {
        const Json value(text);
        const auto expected = boost::json::serialize(value);
        CHECK(canonical_json(value) == expected);
        CHECK(parse_json_strict(canonical_json(value)) == value);
        // A one-key object also compares the key spelling with the original
        // library, including embedded NUL, all controls and Unicode boundaries.
        const Json object = boost::json::object{{text, value}};
        CHECK(canonical_json(object) == boost::json::serialize(object));
        CHECK(parse_json_strict(canonical_json(object), 8U * 1024U * 1024U) == object);
    }
    const std::vector<std::string> invalid{
        "\x80", "\xc0\xaf", "\xc2", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"};
    for (const auto& text : invalid) {
        for (const auto& prefix : {std::string{}, std::string("escaped\n")}) {
            test::throws<Error>([&] { (void)canonical_json(Json(prefix + text)); });
            test::throws<Error>([&] {
                (void)canonical_json(boost::json::object{{prefix + text, "value"}});
            });
        }
    }
}

void canonical_validation_matches_original_dom_parser() {
    // Every fixture constructs object keys in canonical order. The unchanged
    // Boost serializer supplies exact bytes, while the original DOM parse with
    // max_depth 128 independently decides whether those bytes are acceptable.
    const auto check = [](const Json& value) {
        const auto original = boost::json::serialize(value);
        boost::json::parse_options options;
        options.max_depth = 128;
        boost::system::error_code ec;
        static_cast<void>(boost::json::parse(original, ec, {}, options));
        bool rejected = false;
        try {
            const auto actual = canonical_json(value);
            CHECK(!ec);
            CHECK(actual == original);
        } catch (const Error& error) {
            CHECK(error.code() == ErrorCode::InvalidInput);
            rejected = true;
        }
        CHECK(rejected == static_cast<bool>(ec));
    };
    const auto nested = [](std::size_t depth, bool mixed, Json value) {
        for (std::size_t layer = 0; layer < depth; ++layer) {
            if (mixed && layer % 2 == 0) {
                boost::json::object object;
                object.emplace("node", std::move(value));
                value = std::move(object);
            } else {
                boost::json::array array;
                array.push_back(std::move(value));
                value = std::move(array);
            }
        }
        return value;
    };
    for (const std::size_t depth : {126U, 127U, 128U, 129U}) {
        for (const bool mixed : {false, true}) {
            check(nested(depth, mixed, Json{}));
            // The empty innermost container still consumes parser depth even
            // though the append recursion has no child value to visit.
            check(nested(depth, mixed, boost::json::array{}));
            check(nested(depth, mixed, boost::json::object{}));
        }
    }
    for (const Json& value : {Json{}, Json(true), Json(false), Json(std::int64_t{-1}),
             Json(std::numeric_limits<std::int64_t>::min()),
             Json(std::numeric_limits<std::uint64_t>::max()), Json(-0.0),
             Json(std::numeric_limits<double>::denorm_min()),
             Json(std::numeric_limits<double>::max()), Json(1.2345678901234567)}) check(value);
    check(boost::json::object{{"a", boost::json::array{nullptr, true, 1.25,
        boost::json::object{{"a", "line\n"}, {"z", boost::json::array{false, "tail"}}}}}, {"z", "end"}});
    const std::vector<std::string> valid{
        "", "\"\\\b\f\n\r\t/", std::string("nul\0tail", 8),
        "中文😀\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf",
        "early\n" + std::string(1024U * 1024U + 17U, 'p')};
    for (const auto& text : valid) {
        check(Json(text));
        // In particular, the large key must retain the DOM parser's limit,
        // rather than acquiring the strict-input checker's 1 MiB key limit.
        check(boost::json::object{{text, "value"}});
    }
    const std::vector<std::string> invalid{
        "\x80", "\xc0\xaf", "\xc2", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"};
    for (const auto& text : invalid) {
        for (const auto& prefix : {std::string{}, std::string("escaped\n"),
                 std::string("nul\0", 4), std::string(4096, 'p')}) {
            check(Json(prefix + text));
            check(boost::json::object{{prefix + text, "value"}});
        }
        // A valid earlier member must not make the final invalid bytes pass.
        check(boost::json::object{{"a", "valid"}, {"z", "tail" + text}});
    }
    for (const double value : {std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected = false;
        try { (void)canonical_json(Json(value)); }
        catch (const Error& error) { rejected = error.code() == ErrorCode::InvalidInput; }
        CHECK(rejected);
    }
}

void decimal_money_limits_do_not_lose_microdollars() {
    for (int cents = 0; cents <= 20000; ++cents) {
        const auto spelling = std::to_string(cents / 100) + "." +
            (cents % 100 < 10 ? "0" : "") + std::to_string(cents % 100);
        CHECK(cost_limit_micro(parse_json_strict(spelling)) == static_cast<std::int64_t>(cents) * 10000);
    }
    CHECK(cost_limit_micro(parse_json_strict("2.01")) == 2010000);
    CHECK(cost_limit_micro(parse_json_strict("1e-6")) == 1);
    CHECK(cost_limit_micro(parse_json_strict("1e-7")) == 0);
    CHECK(cost_limit_micro(parse_json_strict("1.2345679")) == 1234567);
    CHECK(cost_limit_micro(parse_json_strict("2e2")) == 200000000);
    for (const auto spelling : {"-1", "true", "null", "\"2.01\"", "[]", "1e30"}) {
        bool invalid = false;
        try { (void)cost_limit_micro(parse_json_strict(spelling)); }
        catch (const Error& error) { invalid = error.code() == ErrorCode::InvalidInput; }
        CHECK(invalid);
    }
}

void catalog_shape_errors_are_user_errors() {
    validate_constraint_catalog_shape(parse_json_strict("[]"));
    validate_constraint_catalog_shape(parse_json_strict(R"([{"constraint_id":"c1","text":"original","source_revision":1}])"));
    for (const auto spelling : {R"([{"a":1,"b":2,"c":3}])",
         R"([{"constraint_id":5,"text":"x","source_revision":1}])",
         R"([{"constraint_id":"c1","text":5,"source_revision":1}])",
         R"([{"constraint_id":"c1","text":"x","source_revision":"1"}])",
         R"([{"constraint_id":"c1","text":"x","source_revision":true}])",
         R"([{"constraint_id":"c1","text":"x","source_revision":1.0}])",
         R"([{"constraint_id":"c1","text":"x","source_revision":18446744073709551615}])",
         R"([{"constraint_id":"c1","text":"x","source_revision":0}])", "{}", "[null]"}) {
        bool invalid = false;
        try { validate_constraint_catalog_shape(parse_json_strict(spelling)); }
        catch (const Error& error) { invalid = error.code() == ErrorCode::InvalidInput; }
        CHECK(invalid);
    }
}

void typed_ulids() {
    auto previous = new_id("req");
    CHECK(is_typed_id(previous, "req"));
    for (int i = 0; i < 1000; ++i) {
        const auto next = new_id("req");
        CHECK(next > previous);
        previous = next;
    }
    CHECK(!is_typed_id(previous, "tsk"));
    CHECK(!is_typed_id("req_8ZZZZZZZZZZZZZZZZZZZZZZZZZ", "req"));
    CHECK(!is_typed_id("req_01ARZ3NDEKTSV4RRFFQ69G5FAI", "req"));
    CHECK(!is_typed_id("req_01arz3ndektsv4rrffq69g5fav", "req"));
    test::throws<Error>([] { (void)new_id("not/a/prefix"); });
    test::throws<Error>([] { (void)new_id(""); });
    std::mutex mutex;
    std::set<std::string> ids;
    std::array<std::jthread, 4> threads;
    for (auto& thread : threads) thread = std::jthread([&] {
        for (int i = 0; i < 500; ++i) {
            const auto id = new_id("evt");
            std::lock_guard lock(mutex);
            ids.insert(id);
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(ids.size() == 2000);
}

void binding_round_trip_and_boundaries() {
    ExecutionBinding request;
    request.request_id = new_id("req");
    request.execution_epoch = 0;
    CHECK(execution_binding_from_json(to_json(request)) == request);
    auto task = request;
    task.task_id = new_id("tsk"); task.team_id = new_id("team");
    task.plan_revision = 1; task.task_revision = 1; task.execution_epoch = 1;
    CHECK(execution_binding_from_json(to_json(task)) == task);
    task.instruction_revision = 2; task.requirements_revision = 3;
    CHECK(execution_binding_from_json(parse_json_strict(canonical_json(to_json(task)))) == task);
    auto forged = to_json(task);
    forged.as_object()["extra_authority"] = true;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(task); forged.as_object()["policy_epoch"] = std::numeric_limits<std::uint64_t>::max();
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(task); forged.as_object()["task_revision"] = 1.0;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(request); forged.as_object()["instruction_revision"] = 1;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    for (const auto name : {"request_id", "task_id", "team_id"}) {
        forged = to_json(task); forged.as_object()[name] = "nonempty-but-not-a-typed-id";
        test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    }
    forged = to_json(task); forged.as_object()["task_id"] = task.team_id;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(task); forged.as_object()["request_id"] = task.request_id + std::string(1, '\0');
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    CHECK(role_from_string("team_lead") == wls::Role::TeamLead);
    test::throws<Error>([] { (void)role_from_string("admin"); });
}
} // namespace

int main() {
    return test::run({
        {"strict JSON rejects duplicates, bad UTF-8 and non-finite input", strict_json},
        {"canonical bytes and SHA-256 preserve content identity", canonical_and_digest},
        {"canonical string spans preserve Boost bytes and UTF-8 rejection", canonical_strings_keep_original_boost_bytes_and_utf8_guards},
        {"canonical validation matches original DOM depth and complete UTF-8 bytes", canonical_validation_matches_original_dom_parser},
        {"decimal money limits preserve every cent and round down sub-micro amounts", decimal_money_limits_do_not_lose_microdollars},
        {"catalog input shape rejects missing keys and wrong types as invalid input", catalog_shape_errors_are_user_errors},
        {"typed ULIDs are monotonic and unique across threads", typed_ulids},
        {"execution binding round trips keep request and task scopes distinct", binding_round_trip_and_boundaries}
    });
}
